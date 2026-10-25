#define COBJMACROS
#include <windows.h>
#include <d3d10_1.h>

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

static void swap_chains(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = procedure;
	type.lpszClassName = L"RavenDXMT10";
	type.hInstance = GetModuleHandleW(NULL);
	RegisterClassW(&type);
	HWND window = CreateWindowExW(0, type.lpszClassName, L"DXMT Direct3D 10", WS_OVERLAPPEDWINDOW,
		100, 100, 160, 160, NULL, NULL, type.hInstance, NULL);
	if (!window) ExitProcess(9);
	ShowWindow(window, SW_SHOW);
	DXGI_SWAP_CHAIN_DESC desc = {{64, 64, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0},
		DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, window, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
	for (int version = 0; version < 2; ++version) {
		ID3D10Device *device = NULL;
		ID3D10Device1 *extended = NULL;
		IDXGISwapChain *swap = NULL;
		HRESULT hr = version ? D3D10CreateDeviceAndSwapChain1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0,
			D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &desc, &swap, &extended) :
			D3D10CreateDeviceAndSwapChain(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &desc, &swap, &device);
		if (version) device = (ID3D10Device *)extended;
		if (FAILED(hr) || !swap || !device) ExitProcess(10);
		ID3D10Texture2D *back = NULL;
		ID3D10RenderTargetView *view = NULL;
		if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D10Texture2D, (void **)&back)) ||
			FAILED(ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)back, NULL, &view))) ExitProcess(11);
		const float color[] = {.25f, .5f, .75f, 1};
		for (int frame = 0; frame < 3; ++frame) {
			ID3D10Device_ClearRenderTargetView(device, view, color);
			if (FAILED(IDXGISwapChain_Present(swap, 0, 0))) ExitProcess(12);
		}
		ID3D10RenderTargetView_Release(view);
		ID3D10Texture2D_Release(back);
		IDXGISwapChain_Release(swap);
		ID3D10Device_Release(device);
	}
	DestroyWindow(window);
}

static void readback(ID3D10Device *device, const wchar_t *path)
{
	D3D10_TEXTURE2D_DESC desc = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT,
		D3D10_BIND_RENDER_TARGET, 0, 0};
	ID3D10Texture2D *target = NULL, *staging = NULL;
	ID3D10RenderTargetView *view = NULL;
	if (FAILED(ID3D10Device_CreateTexture2D(device, &desc, NULL, &target)) ||
		FAILED(ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)target, NULL, &view))) ExitProcess(3);
	const float color[] = {.25f, .5f, .75f, 1};
	ID3D10Device_ClearRenderTargetView(device, view, color);
	desc.Usage = D3D10_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
	if (FAILED(ID3D10Device_CreateTexture2D(device, &desc, NULL, &staging))) ExitProcess(4);
	ID3D10Device_CopyResource(device, (ID3D10Resource *)staging, (ID3D10Resource *)target);
	D3D10_MAPPED_TEXTURE2D mapped;
	if (FAILED(ID3D10Texture2D_Map(staging, 0, D3D10_MAP_READ, 0, &mapped))) ExitProcess(5);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	for (unsigned y = 0; y < 4; ++y)
		if (!WriteFile(file, (const BYTE *)mapped.pData + y * mapped.RowPitch, 16, &written, NULL) || written != 16) ExitProcess(6);
	CloseHandle(file);
	ID3D10Texture2D_Unmap(staging, 0);
	ID3D10RenderTargetView_Release(view);
	ID3D10Texture2D_Release(staging);
	ID3D10Texture2D_Release(target);
}

void entry(void)
{
	ID3D10Device *device = NULL;
	ID3D10Device1 *device1 = NULL;
	if (D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, 0, &device) != E_INVALIDARG || device) ExitProcess(1);
	if (FAILED(D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &device)) || !device) ExitProcess(2);
	readback(device, L"C:\\device10.raw");
	ID3D10Device_Release(device);
	if (FAILED(D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_10_1,
		D3D10_1_SDK_VERSION, &device1)) || !device1) ExitProcess(7);
	if (ID3D10Device1_GetFeatureLevel(device1) != D3D10_FEATURE_LEVEL_10_1) ExitProcess(8);
	readback((ID3D10Device *)device1, L"C:\\device10_1.raw");
	ID3D10Device1_Release(device1);
	swap_chains();
	const char text[] = "OK: D3D10 and D3D10.1 hardware devices, clear/copy/map and invalid SDK rejection";
	HANDLE file = CreateFileW(L"C:\\device10.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	WriteFile(file, text, sizeof(text) - 1, &written, NULL);
	CloseHandle(file);
	ExitProcess(0);
}
