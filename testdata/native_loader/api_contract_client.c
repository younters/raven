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
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL), readable;
	if (!mapping || !DuplicateHandle(GetCurrentProcess(), mapping, GetCurrentProcess(), &readable,
		GENERIC_READ | FILE_MAP_READ, FALSE, 0)) return 11;
	void *view = MapViewOfFile(readable, FILE_MAP_READ, 0, 0, 4096);
	if (!view || !UnmapViewOfFile(view)) return 12;
	if (MapViewOfFile(readable, FILE_MAP_WRITE, 0, 0, 4096)) return 13;
	HANDLE writable;
	if (DuplicateHandle(GetCurrentProcess(), readable, GetCurrentProcess(), &writable, GENERIC_WRITE, FALSE, 0) || GetLastError() != ERROR_ACCESS_DENIED) return 14;
	HMODULE setup = LoadLibraryA("setupapi.dll");
	HANDLE (WINAPI *device_set)(const void *, const WCHAR *, HWND, DWORD) = (void *)GetProcAddress(setup, "SetupDiGetClassDevsW");
	BOOL (WINAPI *enum_device)(HANDLE, DWORD, void *) = (void *)GetProcAddress(setup, "SetupDiEnumDeviceInfo");
	BOOL (WINAPI *destroy_set)(HANDLE) = (void *)GetProcAddress(setup, "SetupDiDestroyDeviceInfoList");
	if (!device_set || !enum_device || !destroy_set) return 15;
	HANDLE devices = device_set(NULL, NULL, NULL, 4);
	if (devices == INVALID_HANDLE_VALUE || enum_device(devices, 0, NULL) || GetLastError() != ERROR_NO_MORE_ITEMS || !destroy_set(devices)) return 16;
	HMODULE shell = LoadLibraryA("shell32.dll"), ole = LoadLibraryA("ole32.dll");
	HRESULT (WINAPI *set_id)(const WCHAR *) = (void *)GetProcAddress(shell, "SetCurrentProcessExplicitAppUserModelID");
	HRESULT (WINAPI *get_id)(WCHAR **) = (void *)GetProcAddress(shell, "GetCurrentProcessExplicitAppUserModelID");
	void (WINAPI *free_id)(void *) = (void *)GetProcAddress(ole, "CoTaskMemFree");
	WCHAR *id = (void *)0x100;
	if (!set_id || !get_id || !free_id || get_id(&id) >= 0 || id) return 17;
	if (set_id(L"raven.test") || get_id(&id) || !id || id[0] != L'r' || id[9] != L't' || id[10]) return 18;
	free_id(id);
	BOOL (WINAPI *year_zone)(USHORT, const DYNAMIC_TIME_ZONE_INFORMATION *, TIME_ZONE_INFORMATION *) =
		(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTimeZoneInformationForYear");
	DYNAMIC_TIME_ZONE_INFORMATION rules = {0};
	TIME_ZONE_INFORMATION zone;
	rules.Bias = 300;
	rules.StandardBias = 0;
	rules.DaylightBias = -60;
	rules.DaylightDate.wMonth = 3;
	rules.StandardDate.wMonth = 11;
	if (!year_zone || !year_zone(2026, &rules, &zone) || zone.Bias != 300 || zone.DaylightDate.wMonth != 3) return 19;
	rules.DynamicDaylightTimeDisabled = TRUE;
	if (!year_zone(2026, &rules, &zone) || zone.DaylightDate.wMonth || zone.StandardDate.wMonth || zone.DaylightBias) return 20;
	if (!year_zone(2026, NULL, &zone) || year_zone(0, &rules, &zone) || GetLastError() != ERROR_INVALID_PARAMETER) return 21;
	CloseHandle(readable);
	CloseHandle(mapping);
	return 0;
}
