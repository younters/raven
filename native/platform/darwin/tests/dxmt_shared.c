#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#ifndef DXMT_SHARED_KEYED
#define DXMT_SHARED_KEYED 0
#endif

static int verify(HANDLE shared, int nt)
{
	ID3D11Device *device = NULL;
	ID3D11Device1 *device1 = NULL;
	ID3D11DeviceContext *context = NULL;
	ID3D11Texture2D *texture = NULL, *staging = NULL;
	D3D11_TEXTURE2D_DESC desc;
	D3D11_MAPPED_SUBRESOURCE mapped;
	IDXGIKeyedMutex *mutex = NULL;
	int acquired = 0;
	int result = 10;
	if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
		D3D11_SDK_VERSION, &device, NULL, &context))) return result;
	result = 11;
	if (nt) {
		if (FAILED(ID3D11Device_QueryInterface(device, &IID_ID3D11Device1, (void **)&device1)) ||
			FAILED(ID3D11Device1_OpenSharedResource1(device1, shared, &IID_ID3D11Texture2D, (void **)&texture))) goto done;
	} else if (FAILED(ID3D11Device_OpenSharedResource(device, shared, &IID_ID3D11Texture2D, (void **)&texture))) goto done;
	if (DXMT_SHARED_KEYED) {
		result = 50;
        if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&mutex))) goto done;
        const ULONGLONG started = GetTickCount64();
        if (IDXGIKeyedMutex_AcquireSync(mutex, 99, 25) != WAIT_TIMEOUT || GetTickCount64() - started < 15) {
            result = 54;
            goto done;
        }
        if (IDXGIKeyedMutex_AcquireSync(mutex, 1, INFINITE) != S_OK) goto done;
        acquired = 1;
	}
	ID3D11Texture2D_GetDesc(texture, &desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	result = 12;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging))) goto done;
	ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
	result = 13;
	if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) goto done;
	result = 14;
	if (((unsigned char *)mapped.pData)[0] == 64 && ((unsigned char *)mapped.pData)[1] == 128 &&
		((unsigned char *)mapped.pData)[2] == 191 && ((unsigned char *)mapped.pData)[3] == 255) result = 0;
	ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
done:
	if (acquired && IDXGIKeyedMutex_ReleaseSync(mutex, 1) != S_OK) result = 51;
	if (mutex) IDXGIKeyedMutex_Release(mutex);
	if (staging) ID3D11Texture2D_Release(staging);
	if (texture) ID3D11Texture2D_Release(texture);
	if (device1) ID3D11Device1_Release(device1);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	return result;
}

static void write_result(const char *text)
{
	HANDLE file = CreateFileW(L"C:\\dxmt-shared.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	if (file != INVALID_HANDLE_VALUE) {
		WriteFile(file, text, lstrlenA(text), &written, NULL);
		CloseHandle(file);
	}
}

void entry(void)
{
	WCHAR *line = GetCommandLineW();
	while (*line && *line != L' ') ++line;
	if (*line) {
		int nt = 0;
		int remote = 0;
		for (WCHAR *cursor = line; *cursor && (*cursor < L'0' || *cursor > L'9'); ++cursor)
			if (*cursor == L'n' && cursor[1] == L't') nt = 1;
			else if (*cursor == L'r' && cursor[1] == L'e') remote = nt = 1;
		if (remote) {
			HANDLE file = INVALID_HANDLE_VALUE;
			for (unsigned i = 0; i < 300 && file == INVALID_HANDLE_VALUE; ++i) {
				file = CreateFileW(L"C:\\dxmt-share-remote.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
				if (file == INVALID_HANDLE_VALUE) Sleep(100);
			}
			if (file == INVALID_HANDLE_VALUE) ExitProcess(39);
			char text[32] = {0};
			DWORD length = 0;
			if (!ReadFile(file, text, sizeof(text) - 1, &length, NULL)) ExitProcess(40);
			CloseHandle(file);
			unsigned long value = 0;
			for (unsigned i = 0; i < length && text[i] >= '0' && text[i] <= '9'; ++i)
				value = value * 10 + text[i] - '0';
			ExitProcess(verify((HANDLE)(ULONG_PTR)value, 1));
		}
		unsigned long value = 0;
		while (*line && (*line < L'0' || *line > L'9')) ++line;
		while (*line >= L'0' && *line <= L'9') value = value * 10 + *line++ - L'0';
		ExitProcess(verify((HANDLE)(ULONG_PTR)value, nt));
	}
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	ID3D11Texture2D *texture = NULL;
	ID3D11RenderTargetView *target = NULL;
	IDXGIKeyedMutex *mutex = NULL;
	IDXGIResource *resource = NULL;
	IDXGIResource1 *resource1 = NULL;
	HANDLE shared = NULL;
	int result = 1;
	if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
		D3D11_SDK_VERSION, &device, NULL, &context))) goto done;
	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = desc.Height = 4;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = DXMT_SHARED_KEYED ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
	result = 2;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture))) goto done;
	result = 3;
	if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &target))) goto done;
	if (DXMT_SHARED_KEYED) {
		result = 52;
		if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&mutex)) ||
			IDXGIKeyedMutex_AcquireSync(mutex, 0, 5000) != S_OK) goto done;
	}
	const float clear[] = {64.0f / 255, 128.0f / 255, 191.0f / 255, 1};
	ID3D11DeviceContext_ClearRenderTargetView(context, target, clear);
	ID3D11DeviceContext_Flush(context);
	if (mutex && IDXGIKeyedMutex_ReleaseSync(mutex, 1) != S_OK) { result = 53; goto done; }
	result = 4;
	if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIResource, (void **)&resource))) goto done;
	result = 5;
	if (FAILED(IDXGIResource_GetSharedHandle(resource, &shared)) || !shared) goto done;
	result = 6 + verify(shared, 0);
	if (result != 6) goto done;
	WCHAR path[MAX_PATH], command[MAX_PATH + 64];
	if (!GetModuleFileNameW(NULL, path, MAX_PATH)) { result = 20; goto done; }
	lstrcpyW(command, path);
	lstrcatW(command, L" --child ");
	WCHAR digits[16];
	unsigned long value = (unsigned long)(ULONG_PTR)shared;
	unsigned n = 0;
	do { digits[n++] = L'0' + value % 10; value /= 10; } while (value);
	while (n) { WCHAR digit[] = {digits[--n], 0}; lstrcatW(command, digit); }
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	result = 21;
	if (!CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) goto done;
	result = 22;
	if (WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0) {
		DWORD exit_code = 0;
		if (GetExitCodeProcess(process.hProcess, &exit_code)) result = (int)exit_code;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	if (result) goto done;
	if (DXMT_SHARED_KEYED) {
		write_result("OK: DXMT keyed shared texture synchronized across processes with pixel readback");
		goto done;
	}
	IDXGIResource_Release(resource); resource = NULL;
	ID3D11RenderTargetView_Release(target); target = NULL;
	ID3D11Texture2D_Release(texture); texture = NULL;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	result = 30;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture))) goto done;
	result = 31;
	if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &target))) goto done;
	ID3D11DeviceContext_ClearRenderTargetView(context, target, clear);
	ID3D11DeviceContext_Flush(context);
	result = 32;
	if (FAILED(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIResource1, (void **)&resource1))) goto done;
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	result = 33;
	if (FAILED(IDXGIResource1_CreateSharedHandle(resource1, &attributes, GENERIC_ALL, NULL, &shared)) || !shared) goto done;
	result = 34 + verify(shared, 1);
	if (result != 34) goto done;
	HANDLE copy = NULL;
	result = 35;
	if (!DuplicateHandle(GetCurrentProcess(), shared, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
	result = 36 + verify(copy, 1);
	CloseHandle(copy);
	if (result != 36) goto done;
	lstrcpyW(command, path);
	lstrcatW(command, L" --nt ");
	value = (unsigned long)(ULONG_PTR)shared;
	n = 0;
	do { digits[n++] = L'0' + value % 10; value /= 10; } while (value);
	while (n) { WCHAR digit[] = {digits[--n], 0}; lstrcatW(command, digit); }
	result = 37;
	if (!CreateProcessW(path, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) goto done;
	result = 38;
	if (WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0) {
		DWORD exit_code = 0;
		if (GetExitCodeProcess(process.hProcess, &exit_code)) result = (int)exit_code;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	if (result) goto done;
	lstrcpyW(command, path);
	lstrcatW(command, L" --remote");
	result = 41;
	if (!CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) goto done;
	HANDLE remote = NULL;
	result = 42;
	if (!DuplicateHandle(GetCurrentProcess(), shared, process.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) goto remote_done;
	char remote_text[32];
	unsigned long remote_value = (unsigned long)(ULONG_PTR)remote;
	unsigned remote_length = 0;
	do { remote_text[remote_length++] = '0' + remote_value % 10; remote_value /= 10; } while (remote_value);
	for (unsigned i = 0; i < remote_length / 2; ++i) {
		char digit = remote_text[i]; remote_text[i] = remote_text[remote_length - i - 1]; remote_text[remote_length - i - 1] = digit;
	}
	HANDLE file = CreateFileW(L"C:\\dxmt-share-remote.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
	result = 43;
	if (file == INVALID_HANDLE_VALUE) goto remote_done;
	DWORD written = 0;
	if (!WriteFile(file, remote_text, remote_length, &written, NULL) || written != remote_length) {
		CloseHandle(file); goto remote_done;
	}
	CloseHandle(file);
	result = 44;
	if (WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0) {
		DWORD exit_code = 0;
		if (GetExitCodeProcess(process.hProcess, &exit_code)) result = (int)exit_code;
	}
remote_done:
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(shared);
	if (!result) write_result("OK: DXMT legacy and NT shared textures, inheritance and DuplicateHandle across processes with pixel readback");
done:
	if (mutex) IDXGIKeyedMutex_Release(mutex);
	if (resource1) IDXGIResource1_Release(resource1);
	if (resource) IDXGIResource_Release(resource);
	if (target) ID3D11RenderTargetView_Release(target);
	if (texture) ID3D11Texture2D_Release(texture);
	if (context) ID3D11DeviceContext_Release(context);
	if (device) ID3D11Device_Release(device);
	ExitProcess(result);
}
