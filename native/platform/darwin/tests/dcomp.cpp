// SPDX-License-Identifier: GPL-3.0-or-later
#define _WIN32_WINNT 0x0a00
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <windows.h>

static bool argument(const wchar_t *word) {
	for (const wchar_t *s = GetCommandLineW(); *s; ++s) {
		const wchar_t *a = s, *b = word;
		while (*a && *b && *a == *b) {
			++a;
			++b;
		}
		if (!*b)
			return true;
	}
	return false;
}
static void check(HRESULT hr, int code) {
	if (FAILED(hr))
		ExitProcess(code);
}
static void pump() {
	MSG message;
	while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}
static void stage(char digit) {
	char path[] = "C:\\dcomp-stage0";
	path[14] = digit;
	DWORD written;
	HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "OK", 2, &written, nullptr))
		ExitProcess(40);
	CloseHandle(file);
	char next[] = "C:\\dcomp-next0";
	next[13] = digit;
	for (unsigned i = 0; i < 400; ++i) {
		if (GetFileAttributesA(next) != INVALID_FILE_ATTRIBUTES)
			return;
		pump();
		Sleep(25);
	}
	ExitProcess(41);
}
struct ConcurrentTree {
	IDCompositionVisual2 *root;
	IDCompositionVisual2 *child;
};
static DWORD WINAPI change_tree(void *argument) {
	auto tree = (ConcurrentTree *)argument;
	for (unsigned i = 0; i < 64; ++i) {
		check(tree->root->AddVisual(tree->child, TRUE, nullptr), 81);
		check(tree->child->SetOffsetX(float(i)), 82);
		check(tree->root->RemoveVisual(tree->child), 83);
	}
	return 0;
}
static void render(HWND window) {
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
							D3D11_SDK_VERSION, &device, nullptr, &context),
		  2);
	IDXGIDevice *dxgi = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory2 *factory = nullptr;
	check(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi), 3);
	check(dxgi->GetAdapter(&adapter), 4);
	check(adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory), 5);
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = 200;
	desc.Height = 120;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	IDXGISwapChain1 *swap = nullptr;
	check(factory->CreateSwapChainForComposition(device, &desc, nullptr, &swap), 6);
	HWND none = (HWND)1;
	if (swap->GetHwnd(&none) != DXGI_ERROR_INVALID_CALL || none)
		ExitProcess(7);
	IDCompositionDevice2 *composition = nullptr;
	check(DCompositionCreateDevice2(dxgi, __uuidof(IDCompositionDevice2), (void **)&composition), 8);
	IDCompositionDesktopDevice *desktop = nullptr;
	check(composition->QueryInterface(__uuidof(IDCompositionDesktopDevice), (void **)&desktop), 9);
	IDCompositionTarget *target = nullptr;
	check(desktop->CreateTargetForHwnd(window, TRUE, &target), 10);
	IDCompositionVisual2 *visual = nullptr;
	check(composition->CreateVisual(&visual), 11);
	check(visual->SetContent(swap), 12);
	check(visual->SetOffsetX(25.0f), 13);
	check(visual->SetOffsetY(20.0f), 14);
	check(target->SetRoot(visual), 15);
	check(composition->Commit(), 16);
	// Mutating a tree on another thread must not invalidate Commit's traversal.
	IDCompositionVisual2 *concurrent_child = nullptr;
	check(composition->CreateVisual(&concurrent_child), 84);
	ConcurrentTree tree{visual, concurrent_child};
	HANDLE worker = CreateThread(nullptr, 0, change_tree, &tree, 0, nullptr);
	if (!worker)
		ExitProcess(85);
	for (unsigned i = 0; i < 64; ++i)
		check(composition->Commit(), 86);
	if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0)
		ExitProcess(87);
	CloseHandle(worker);
	concurrent_child->Release();
	IDCompositionDevice2 *other_device = nullptr;
	IDCompositionRectangleClip *other_clip = nullptr;
	check(DCompositionCreateDevice2(dxgi, __uuidof(IDCompositionDevice2), (void **)&other_device), 88);
	check(other_device->CreateRectangleClip(&other_clip), 89);
	if (visual->SetClip(other_clip) != E_INVALIDARG)
		ExitProcess(90);
	other_clip->Release();
	other_device->Release();
	if (target->SetRoot((IDCompositionVisual *)composition) != E_INVALIDARG ||
		visual->SetClip((IDCompositionClip *)composition) != E_INVALIDARG ||
		visual->AddVisual((IDCompositionVisual *)composition, TRUE, nullptr) != E_INVALIDARG)
		ExitProcess(79);
	if (visual->SetBitmapInterpolationMode((DCOMPOSITION_BITMAP_INTERPOLATION_MODE)2) != E_INVALIDARG ||
		visual->SetBorderMode((DCOMPOSITION_BORDER_MODE)2) != E_INVALIDARG ||
		visual->SetOpacityMode((DCOMPOSITION_OPACITY_MODE)2) != E_INVALIDARG ||
		visual->SetBackFaceVisibility((DCOMPOSITION_BACKFACE_VISIBILITY)2) != E_INVALIDARG)
		ExitProcess(42);
	check(visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_INHERIT), 43);
	ID3D11Texture2D *back = nullptr;
	ID3D11RenderTargetView *view = nullptr;
	check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back), 17);
	check(device->CreateRenderTargetView(back, nullptr, &view), 18);
	const float green[] = {0, 1, 0, 1};
	for (unsigned i = 0; i < 60; ++i) {
		context->ClearRenderTargetView(view, green);
		check(swap->Present(0, 0), 19);
		if (!i) {
			IDXGIDevice2 *extended = nullptr;
			check(dxgi->QueryInterface(__uuidof(IDXGIDevice2), (void **)&extended), 20);
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			check(extended->EnqueueSetEvent(event), 21);
			if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
				ExitProcess(22);
			CloseHandle(event);
			extended->Release();
		}
		pump();
		Sleep(25);
	}
	stage('1');
	check(visual->SetOffsetX(80.0f), 23);
	check(visual->SetOffsetY(60.0f), 24);
	stage('2'); // Setters must not change the committed visual.
	check(composition->Commit(), 25);
	stage('3'); // No new swap-chain Present.
	D2D_MATRIX_3X2_F scale = {1.5f, 0, 0, .5f, 0, 0};
	check(visual->SetTransform(scale), 44);
	check(visual->SetClip(D2D_RECT_F{20, 10, 180, 110}), 45);
	check(composition->Commit(), 46);
	stage('4');
	check(visual->SetTransform((IDCompositionTransform *)nullptr), 47);
	IDCompositionRectangleClip *clip = nullptr;
	check(composition->CreateRectangleClip(&clip), 48);
	check(clip->SetLeft(0.0f), 49);
	check(clip->SetTop(0.0f), 50);
	check(clip->SetRight(200.0f), 51);
	check(clip->SetBottom(120.0f), 52);
	check(clip->SetTopLeftRadiusX(20.0f), 53);
	check(clip->SetTopLeftRadiusY(20.0f), 54);
	check(clip->SetTopRightRadiusX(20.0f), 55);
	check(clip->SetTopRightRadiusY(20.0f), 56);
	check(clip->SetBottomLeftRadiusX(20.0f), 57);
	check(clip->SetBottomLeftRadiusY(20.0f), 58);
	check(clip->SetBottomRightRadiusX(20.0f), 59);
	check(clip->SetBottomRightRadiusY(20.0f), 60);
	check(visual->SetClip(clip), 61);
	check(composition->Commit(), 62);
	stage('5');
	IDCompositionVisual2 *root = nullptr, *overlay = nullptr;
	check(composition->CreateVisual(&root), 63);
	check(composition->CreateVisual(&overlay), 64);
	check(root->AddVisual(visual, FALSE, nullptr), 65);
	check(root->AddVisual(overlay, TRUE, nullptr), 66);
	check(target->SetRoot(root), 67);
	desc.Width = 100;
	desc.Height = 80;
	IDXGISwapChain1 *red_swap = nullptr;
	check(factory->CreateSwapChainForComposition(device, &desc, nullptr, &red_swap), 68);
	check(overlay->SetContent(red_swap), 69);
	check(overlay->SetOffsetX(120.0f), 70);
	check(overlay->SetOffsetY(80.0f), 71);
	ID3D11Texture2D *red_back = nullptr;
	ID3D11RenderTargetView *red_view = nullptr;
	check(red_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&red_back), 72);
	check(device->CreateRenderTargetView(red_back, nullptr, &red_view), 73);
	const float half_red[] = {.5f, 0, 0, .5f};
	context->ClearRenderTargetView(red_view, half_red);
	check(red_swap->Present(0, 0), 74);
	check(composition->Commit(), 75);
	stage('6');
	check(root->RemoveVisual(overlay), 76);
	check(composition->Commit(), 77);
	stage('7');
	HWND resize_window = argument(L"--child") ? GetParent(window) : window;
	const int factor = argument(L"--hidpi") ? 2 : 1;
	if (!SetWindowPos(resize_window, nullptr, 0, 0, 640 * factor, 420 * factor,
					  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
		ExitProcess(78);
	pump();
	stage('8');
	red_view->Release();
	red_back->Release();
	view->Release();
	back->Release();
	target->Release();
	root->Release();
	overlay->Release();
	visual->Release();
	clip->Release();
	desktop->Release();
	composition->Release();
	red_swap->Release();
	swap->Release();
	factory->Release();
	adapter->Release();
	dxgi->Release();
	context->Release();
	device->Release();
	DestroyWindow(window);
	ExitProcess(0);
}
extern "C" void entry() {
	if (argument(L"--hidpi"))
		SetProcessDPIAware();
	if (argument(L"--child")) {
		HWND hidden = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
		HWND child =
			CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hidden, nullptr, nullptr, nullptr);
		if (!child)
			ExitProcess(31);
		DWORD written;
		HANDLE file = CreateFileW(L"C:\\dcomp-child.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE || !WriteFile(file, &child, sizeof(child), &written, nullptr))
			ExitProcess(32);
		CloseHandle(file);
		render(child);
	}
	HWND window = CreateWindowExW(0, L"STATIC", L"Raven DirectComposition", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
								  400, 300, nullptr, nullptr, nullptr, nullptr);
	if (!window)
		ExitProcess(1);
	if (argument(L"--popup")) {
		HWND owner = window;
		window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Raven composition popup",
			WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, owner, nullptr, nullptr, nullptr);
		if (!window) ExitProcess(79);
	}
	if (!argument(L"--foreign"))
		render(window);
	wchar_t path[1024], command[1100];
	if (!GetModuleFileNameW(nullptr, path, 1024))
		ExitProcess(33);
	size_t at = 0;
	command[at++] = L'"';
	for (size_t i = 0; path[i]; ++i)
		command[at++] = path[i];
	command[at++] = L'"';
	for (const wchar_t *q = L" --child"; *q; ++q)
		command[at++] = *q;
	if (argument(L"--hidpi"))
		for (const wchar_t *q = L" --hidpi"; *q; ++q)
			command[at++] = *q;
	command[at] = 0;
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(path, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
		ExitProcess(34);
	HWND child = nullptr;
	for (unsigned i = 0; i < 400 && !child; ++i) {
		HANDLE file =
			CreateFileW(L"C:\\dcomp-child.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			DWORD read = 0;
			ReadFile(file, &child, sizeof(child), &read, nullptr);
			CloseHandle(file);
			if (read != sizeof(child))
				child = nullptr;
		}
		pump();
		Sleep(25);
	}
	if (!child)
		ExitProcess(35);
	SetLastError(0);
	HWND previous = SetParent(child, window);
	if (!previous && GetLastError())
		ExitProcess(36);
	if (!SetWindowPos(child, nullptr, 0, 0, 320, 200, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW))
		ExitProcess(37);
	while (WaitForSingleObject(process.hProcess, 25) == WAIT_TIMEOUT)
		pump();
	DWORD code = 99;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	DestroyWindow(window);
	ExitProcess(code);
}
