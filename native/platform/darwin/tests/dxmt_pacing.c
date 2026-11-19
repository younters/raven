#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

void entry(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = procedure;
	type.lpszClassName = L"RavenDXMTPacing";
	type.hInstance = GetModuleHandleW(NULL);
	RegisterClassW(&type);
	HWND window = CreateWindowExW(0, type.lpszClassName, L"DXMT present pacing", WS_OVERLAPPEDWINDOW,
		100, 100, 160, 160, NULL, NULL, type.hInstance, NULL);
	ShowWindow(window, SW_SHOW);
	DXGI_SWAP_CHAIN_DESC desc = {{64, 64, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0},
		DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, window, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
	IDXGISwapChain *swap = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
		&desc, &swap, &device, NULL, &context))) ExitProcess(1);
	ID3D11Texture2D *back = NULL;
	ID3D11RenderTargetView *view = NULL;
	if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back)) ||
		FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &view))) ExitProcess(2);
	LARGE_INTEGER start, end, frequency;
	QueryPerformanceFrequency(&frequency);
	const ULONGLONG tick_start = GetTickCount64();
	QueryPerformanceCounter(&start);
	for (int frame = 0; frame < 16; ++frame) {
		const float color[] = {.25f, .5f, .75f, 1};
		ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
		if (FAILED(IDXGISwapChain_Present(swap, 4, 0))) ExitProcess(3);
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
	}
	// Present queues work asynchronously. Wait for the following GPU event so this measures execution through the bounded pool.
	D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
	ID3D11Query *query = NULL;
	if (FAILED(ID3D11Device_CreateQuery(device, &query_desc, &query))) ExitProcess(4);
	ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
	ID3D11DeviceContext_Flush(context);
	BOOL done = FALSE;
	HRESULT status;
	while ((status = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query, &done, sizeof done, 0)) == S_FALSE)
		Sleep(1);
	if (FAILED(status) || !done) ExitProcess(5);
	ID3D11Query_Release(query);
	QueryPerformanceCounter(&end);
	const ULONG elapsed = (ULONG)((end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
	const ULONGLONG tick_elapsed = (GetTickCount64() - tick_start) * 1000;
	if ((ULONGLONG)elapsed + 100000 < tick_elapsed || (ULONGLONG)elapsed > tick_elapsed + 100000) ExitProcess(6);
	Sleep(250);
	ID3D11RenderTargetView_Release(view);
	ID3D11Texture2D_Release(back);
	IDXGISwapChain_Release(swap);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	char text[64];
	int size = wsprintfA(text, "16 %lu", elapsed);
	HANDLE file = CreateFileW(L"C:\\pacing.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	WriteFile(file, text, size, &written, NULL);
	CloseHandle(file);
	ExitProcess(0);
}
