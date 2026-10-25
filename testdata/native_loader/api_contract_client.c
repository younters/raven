#include <windows.h>

static unsigned initialized;
static BOOL WINAPI initialize(void *once, void *parameter, void **context)
{
	++initialized;
	*context = parameter;
	return TRUE;
}

UINT raven_entry(void)
{
	HMODULE version = LoadLibraryA("api-ms-win-core-version-l1-1-1.dll");
	HMODULE psapi = LoadLibraryA("api-ms-win-core-psapi-l1-1-0.dll");
	HMODULE threads = LoadLibraryA("api-ms-win-core-processthreads-l1-1-0.dll");
	BOOL (WINAPI *modules)(HANDLE, HMODULE *, DWORD, DWORD *) = (void *)GetProcAddress(psapi, "K32EnumProcessModules");
	BOOL (WINAPI *token)(HANDLE, DWORD, HANDLE *) = (void *)GetProcAddress(threads, "OpenProcessToken");
	HMODULE images[256];
	DWORD required;
	HANDLE handle;
	if (!version || !psapi || !threads || !modules || !token) return 1;
	if (!GetProcAddress(version, "GetFileVersionInfoSizeW") || !GetProcAddress(version, "GetFileVersionInfoW") ||
		!GetProcAddress(version, "VerQueryValueW")) return 2;
	if (!modules(GetCurrentProcess(), images, sizeof(images), &required) || required < sizeof(HMODULE)) return 3;
	if (!token(GetCurrentProcess(), TOKEN_QUERY, &handle) || !CloseHandle(handle)) return 4;
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	void *(__cdecl *copy)(void *, const void *, size_t) = (void *)GetProcAddress(nt, "memcpy");
	void *(__cdecl *fill)(void *, int, size_t) = (void *)GetProcAddress(nt, "memset");
	SIZE_T (WINAPI *compare)(const void *, const void *, SIZE_T) = (void *)GetProcAddress(nt, "RtlCompareMemory");
	void (WINAPI *set_error)(DWORD) = (void *)GetProcAddress(nt, "RtlSetLastWin32Error");
	LONG (WINAPI *once)(void *, void *, void *, void **) = (void *)GetProcAddress(nt, "RtlRunOnceExecuteOnce");
	void (WINAPI *acquire)(void *) = (void *)GetProcAddress(nt, "RtlAcquireSRWLockExclusive");
	void (WINAPI *release)(void *) = (void *)GetProcAddress(nt, "RtlReleaseSRWLockExclusive");
	ULONG_PTR state = 0, lock = 0;
	void *context = NULL;
	char bytes[5];
	if (!copy || !fill || !compare || !set_error || !once || !acquire || !release) return 5;
	if (fill(bytes, 'a', sizeof(bytes)) != bytes || copy(bytes, "abcd", 4) != bytes || compare(bytes, "abce", 4) != 3) return 6;
	set_error(1234);
	if (GetLastError() != 1234) return 7;
	if (once(&state, initialize, (void *)0x100, &context) || context != (void *)0x100 || initialized != 1) return 8;
	if (once(&state, initialize, (void *)0x200, &context) || context != (void *)0x100 || initialized != 1) return 9;
	acquire(&lock);
	release(&lock);
	if (lock) return 10;
	return 0;
}
