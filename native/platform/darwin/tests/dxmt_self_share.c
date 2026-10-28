#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// What Firefox's GPU process checks before it uses a device (D3D11Checks::DoesTextureSharingWork): a keyed-mutex shared texture filled
// under its mutex, opened again on the device that made it, copied to a staging texture under the mutex and read back, given a shader
// view, and every reference released, for BGRA and R8. Firefox releases the mutex twice, the second time from its lock's destructor.
static int check(ID3D11Device *device, ID3D11DeviceContext *context, DXGI_FORMAT format, UINT bind, UINT pixel, UINT bytes)
{
    D3D11_TEXTURE2D_DESC desc = {32, 32, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
    static UINT color[32 * 32];
    for (int i = 0; i < 32 * 32; ++i) color[i] = pixel;
    ID3D11Texture2D *texture = NULL, *shared_texture = NULL, *staging = NULL;
    IDXGIResource *resource = NULL;
    ID3D11Resource *shared = NULL;
    IDXGIKeyedMutex *mutex = NULL, *source_mutex = NULL;
    ID3D11ShaderResourceView *view = NULL;
    HANDLE handle = NULL;
    int code = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture))) goto done;
    code = 12;
    if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&source_mutex)) ||
        IDXGIKeyedMutex_AcquireSync(source_mutex, 0, 30000) != S_OK) goto done;
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)texture, 0, NULL, color, 32 * bytes, 32 * 32 * bytes);
    if (FAILED(IDXGIKeyedMutex_ReleaseSync(source_mutex, 0))) goto done;
    code = 2;
    if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIResource, (void **)&resource)) ||
        FAILED(IDXGIResource_GetSharedHandle(resource, &handle)) || !handle) goto done;
    code = 3;
    if (FAILED(ID3D11Device_OpenSharedResource(device, handle, &IID_ID3D11Resource, (void **)&shared)) ||
        FAILED(ID3D11Resource_QueryInterface(shared, &IID_ID3D11Texture2D, (void **)&shared_texture))) goto done;
    code = 4;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.BindFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging))) goto done;
    code = 5;
    if (FAILED(ID3D11Resource_QueryInterface(shared, &IID_IDXGIKeyedMutex, (void **)&mutex)) ||
        IDXGIKeyedMutex_AcquireSync(mutex, 0, 30000) != S_OK) goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)shared_texture);
    code = 6;
    if (FAILED(IDXGIKeyedMutex_ReleaseSync(mutex, 0))) goto done;
    // Not held any more: the second release fails with E_FAIL, and nothing else changes.
    if (IDXGIKeyedMutex_ReleaseSync(mutex, 0) != E_FAIL) goto done;
    code = 7;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) goto done;
    UINT first = 0;
    for (UINT i = 0; i < bytes; ++i) first |= (UINT)((BYTE *)mapped.pData)[i] << (8 * i);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    code = 8;
    if (first != (bytes == 4 ? pixel : (pixel & 0xff))) goto done;
    code = 9;
    if (bind & D3D11_BIND_SHADER_RESOURCE &&
        FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)shared_texture, NULL, &view))) goto done;
    code = 0;
done:
    if (view) ID3D11ShaderResourceView_Release(view);
    if (mutex) IDXGIKeyedMutex_Release(mutex);
    if (source_mutex) IDXGIKeyedMutex_Release(source_mutex);
    if (staging) ID3D11Texture2D_Release(staging);
    if (shared_texture) ID3D11Texture2D_Release(shared_texture);
    if (shared) ID3D11Resource_Release(shared);
    if (resource) IDXGIResource_Release(resource);
    if (texture) ID3D11Texture2D_Release(texture);
    return code;
}

// D3D11Checks::DoesRenderTargetViewNeedRecreating: an NT-handle keyed texture its creator clears twice under the mutex, copying the second
// clear to a staging texture while it still holds the mutex.
static int recreate(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_TEXTURE2D_DESC desc = {32, 32, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    IDXGIKeyedMutex *mutex = NULL;
    ID3D11RenderTargetView *view = NULL;
    int code = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture)) ||
        FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&mutex))) goto done;
    code = 2;
    D3D11_RENDER_TARGET_VIEW_DESC target = {DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D};
    if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, &target, &view))) goto done;
    code = 3;
    const FLOAT first[4] = {1, 1, 0.5f, 1}, second[4] = {1, 1, 0, 1};
    if (IDXGIKeyedMutex_AcquireSync(mutex, 0, INFINITE) != S_OK) goto done;
    ID3D11DeviceContext_ClearRenderTargetView(context, view, first);
    if (FAILED(IDXGIKeyedMutex_ReleaseSync(mutex, 0))) goto done;
    code = 4;
    if (IDXGIKeyedMutex_AcquireSync(mutex, 0, INFINITE) != S_OK) goto done;
    ID3D11DeviceContext_ClearRenderTargetView(context, view, second);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.BindFlags = 0;
    code = 5;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging))) goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    code = 6;
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) goto done;
    const UINT color = *(UINT *)mapped.pData;
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    staging = NULL;
    code = 7;
    if (color != 0xffffff00U) goto done;
    code = 8;
    if (FAILED(IDXGIKeyedMutex_ReleaseSync(mutex, 0))) goto done;
    code = 0;
done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (view) ID3D11RenderTargetView_Release(view);
    if (mutex) IDXGIKeyedMutex_Release(mutex);
    if (texture) ID3D11Texture2D_Release(texture);
    return code;
}

void entry(void)
{
    int code = 10;
    for (int round = 0; round < 2; ++round) {
        ID3D11Device *device = NULL;
        ID3D11DeviceContext *context = NULL;
        D3D_FEATURE_LEVEL level;
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, &device, &level, &context))) ExitProcess(code);
        if ((code = check(device, context, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                0xffffff80U, 4))) ExitProcess(20 + code);
        if ((code = check(device, context, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, 0x80, 1))) ExitProcess(40 + code);
        if ((code = recreate(device, context))) ExitProcess(60 + code);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        code = 11;
    }
    HANDLE file = CreateFileA("C:\\dxmt-self-share.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    const char text[] = "OK: keyed shared textures reopened on their own device, read back and released";
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof text - 1, &written, NULL)) ExitProcess(12);
    CloseHandle(file);
    ExitProcess(0);
}
