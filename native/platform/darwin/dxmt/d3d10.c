// Public D3D10/10.1 creation entry points for the pinned DXMT d3d10core. Raven code; no Wine DLL is required.
#define COBJMACROS
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi.h>

typedef HRESULT (WINAPI *core_create)(IDXGIFactory *, IDXGIAdapter *, UINT, UINT, ID3D10Device **);

static HRESULT create_device(IDXGIAdapter *given, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags,
	UINT level, REFIID iid, void **out)
{
	if (!out) return E_INVALIDARG;
	*out = NULL;
	if (software || type != D3D10_DRIVER_TYPE_HARDWARE) return DXGI_ERROR_UNSUPPORTED;
	IDXGIFactory *factory = NULL;
	IDXGIAdapter *adapter = given;
	HRESULT hr;
	if (adapter) {
		IDXGIAdapter_AddRef(adapter);
		hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
	} else {
		hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
		if (SUCCEEDED(hr)) hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
	}
	if (SUCCEEDED(hr)) {
		// Keep the module alive for the lifetime of the returned COM vtables. Concurrent first callers retain only one load.
		static HMODULE module;
		HMODULE loaded = InterlockedCompareExchangePointer((void *volatile *)&module, NULL, NULL);
		if (!loaded) {
			loaded = LoadLibraryW(L"d3d10core.dll");
			if (loaded) {
				HMODULE previous = InterlockedCompareExchangePointer((void *volatile *)&module, loaded, NULL);
				if (previous) { FreeLibrary(loaded); loaded = previous; }
			}
		}
		core_create create = loaded ? (core_create)GetProcAddress(loaded, "D3D10CoreCreateDevice") : NULL;
		ID3D10Device *device = NULL;
		hr = create ? create(factory, adapter, flags, level, &device) : DXGI_ERROR_UNSUPPORTED;
		if (SUCCEEDED(hr)) {
			hr = ID3D10Device_QueryInterface(device, iid, out);
			ID3D10Device_Release(device);
		}
	}
	if (adapter) IDXGIAdapter_Release(adapter);
	if (factory) IDXGIFactory_Release(factory);
	return hr;
}

static HRESULT create_swap(IUnknown *device, const DXGI_SWAP_CHAIN_DESC *description, IDXGISwapChain **out)
{
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory *factory = NULL;
	HRESULT hr = IUnknown_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
	if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
	if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
	if (SUCCEEDED(hr)) hr = IDXGIFactory_CreateSwapChain(factory, device, (DXGI_SWAP_CHAIN_DESC *)description, out);
	if (factory) IDXGIFactory_Release(factory);
	if (adapter) IDXGIAdapter_Release(adapter);
	if (dxgi) IDXGIDevice_Release(dxgi);
	return hr;
}

#ifdef RAVEN_D3D10_1
HRESULT WINAPI D3D10CreateDevice1(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags,
	D3D10_FEATURE_LEVEL1 level, UINT sdk, ID3D10Device1 **device)
{
	if (device) *device = NULL;
	if (sdk != D3D10_1_SDK_VERSION || (level != D3D10_FEATURE_LEVEL_10_0 && level != D3D10_FEATURE_LEVEL_10_1)) return E_INVALIDARG;
	return create_device(adapter, type, software, flags, level, &IID_ID3D10Device1, (void **)device);
}

HRESULT WINAPI D3D10CreateDeviceAndSwapChain1(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags,
	D3D10_FEATURE_LEVEL1 level, UINT sdk, DXGI_SWAP_CHAIN_DESC *description, IDXGISwapChain **swap, ID3D10Device1 **out)
{
	if (swap) *swap = NULL;
	if (out) *out = NULL;
	if ((!swap && !out) || (swap && !description)) return E_INVALIDARG;
	ID3D10Device1 *device = NULL;
	HRESULT hr = D3D10CreateDevice1(adapter, type, software, flags, level, sdk, &device);
#else
HRESULT WINAPI D3D10CreateDevice(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags,
	UINT sdk, ID3D10Device **device)
{
	if (device) *device = NULL;
	if (sdk != D3D10_SDK_VERSION) return E_INVALIDARG;
	return create_device(adapter, type, software, flags, D3D10_FEATURE_LEVEL_10_0, &IID_ID3D10Device, (void **)device);
}

HRESULT WINAPI D3D10CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags,
	UINT sdk, DXGI_SWAP_CHAIN_DESC *description, IDXGISwapChain **swap, ID3D10Device **out)
{
	if (swap) *swap = NULL;
	if (out) *out = NULL;
	if ((!swap && !out) || (swap && !description)) return E_INVALIDARG;
	ID3D10Device *device = NULL;
	HRESULT hr = D3D10CreateDevice(adapter, type, software, flags, sdk, &device);
#endif
	if (SUCCEEDED(hr) && swap) hr = create_swap((IUnknown *)device, description, swap);
	if (SUCCEEDED(hr) && out) *out = device;
	else if (device) IUnknown_Release((IUnknown *)device);
	return hr;
}
