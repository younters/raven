#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

void entry(void)
{
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL level = 0;
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
	if (FAILED(hr) || !device || !context || level < D3D_FEATURE_LEVEL_11_0)
		ExitProcess(1);
	D3D11_BUFFER_DESC desc = {256, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
	ID3D11Buffer *buffer = NULL;
	if (FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer)) || !buffer)
		ExitProcess(2);
	ID3D11Buffer_Release(buffer);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	const char text[] = "OK: DXMT D3D11 hardware device, immediate context and vertex buffer";
	HANDLE file = CreateFileW(L"C:\\dxmt-device.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof(text) - 1, &written, NULL) || written != sizeof(text) - 1)
		ExitProcess(3);
	CloseHandle(file);
	ExitProcess(0);
}
