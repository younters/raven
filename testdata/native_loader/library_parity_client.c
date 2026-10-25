#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdarg.h>

static int (__cdecl *format_text)(char *, size_t, const char *, va_list);
static int format(char *buffer, size_t size, const char *text, ...)
{
	va_list arguments;
	va_start(arguments, text);
	const int result = format_text(buffer, size, text, arguments);
	va_end(arguments);
	return result;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	OSVERSIONINFOEXA version_a = {0};
	OSVERSIONINFOEXW version_w = {0};
	version_a.dwOSVersionInfoSize = sizeof version_a;
	version_w.dwOSVersionInfoSize = sizeof version_w;
	if (!GetVersionExA((OSVERSIONINFOA *)&version_a) || !GetVersionExW((OSVERSIONINFOW *)&version_w) ||
		version_a.dwMajorVersion != version_w.dwMajorVersion || version_a.dwMinorVersion != version_w.dwMinorVersion ||
		version_a.dwBuildNumber != version_w.dwBuildNumber || version_a.wProductType != version_w.wProductType) return 22;
	WCHAR program[MAX_PATH], folder[MAX_PATH], expanded[MAX_PATH], architecture[32];
#ifdef _WIN64
	const WCHAR *expected = L"C:\\Program Files";
	const WCHAR *cpu = L"AMD64";
#else
	const WCHAR *expected = L"C:\\Program Files (x86)";
	const WCHAR *cpu = L"x86";
#endif
	if (!GetEnvironmentVariableW(L"ProgramFiles", program, MAX_PATH) || lstrcmpW(program, expected)) return 1;
	if (!GetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", architecture, 32) || lstrcmpW(architecture, cpu)) return 2;
	if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, folder) || lstrcmpW(folder, expected)) return 3;
	if (!ExpandEnvironmentStringsW(L"%ProgramFiles%", expanded, MAX_PATH) || lstrcmpW(expanded, expected)) return 4;
	if (!SetEnvironmentVariableA("RAVEN_PARITY", "one")) return 5;
	LPCH block = GetEnvironmentStrings(), at = block;
	BOOL found = FALSE;
	if (!at) return 6;
	while (*at) {
		if (!lstrcmpA(at, "RAVEN_PARITY=one")) found = TRUE;
		at += lstrlenA(at) + 1;
	}
	if (!found || !FreeEnvironmentStringsA(block)) return 7;
	LPWCH wide = GetEnvironmentStringsW(), scan = wide;
	found = FALSE;
	if (!scan) return 8;
	while (*scan) {
		if (!lstrcmpW(scan, L"RAVEN_PARITY=one")) found = TRUE;
		scan += lstrlenW(scan) + 1;
	}
	if (!found || !FreeEnvironmentStringsW(wide)) return 9;
	HKEY key;
	DWORD bytes = sizeof folder, type;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion", 0, KEY_QUERY_VALUE, &key) ||
		RegQueryValueExW(key, L"ProgramFilesDir", NULL, &type, (BYTE *)folder, &bytes) || type != REG_SZ || lstrcmpW(folder, expected)) return 10;
	RegCloseKey(key);

	struct { URL_COMPONENTSW value; DWORD guard; } url = {{0}, 0x12345678};
	WCHAR scheme[16], host[64], user[32], password[32], path[64], extra[64];
	url.value.dwStructSize = sizeof url.value;
	url.value.lpszScheme = scheme; url.value.dwSchemeLength = 16;
	url.value.lpszHostName = host; url.value.dwHostNameLength = 64;
	url.value.lpszUserName = user; url.value.dwUserNameLength = 32;
	url.value.lpszPassword = password; url.value.dwPasswordLength = 32;
	url.value.lpszUrlPath = path; url.value.dwUrlPathLength = 64;
	url.value.lpszExtraInfo = extra; url.value.dwExtraInfoLength = 64;
	if (!InternetCrackUrlW(L"https://name:secret@example.test:8443/folder?q=1", 0, 0, &url.value)) return 11;
	if (url.guard != 0x12345678 || url.value.nScheme != INTERNET_SCHEME_HTTPS || url.value.nPort != 8443 ||
		lstrcmpW(host, L"example.test") || lstrcmpW(user, L"name") || lstrcmpW(password, L"secret") ||
		lstrcmpW(path, L"/folder") || lstrcmpW(extra, L"?q=1") || url.value.dwHostNameLength != 12) return 12;
	HMODULE http = LoadLibraryW(L"winhttp.dll");
	BOOL (WINAPI *proxy)(void *) = (void *)GetProcAddress(http, "WinHttpGetIEProxyConfigForCurrentUser");
	struct { BOOL detect; WCHAR *url, *proxy, *bypass; } config;
	struct { typeof(config) value; DWORD guard; } guarded = {{0}, 0x87654321};
	if (!proxy || !proxy(&guarded.value) || guarded.guard != 0x87654321 || guarded.value.url || guarded.value.proxy || guarded.value.bypass) return 13;
	WSADATA sockets;
	if (WSAStartup(MAKEWORD(2, 2), &sockets)) return 14;
	SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_handle == INVALID_SOCKET || closesocket(socket_handle) || WSACleanup()) return 15;
	DATA_BLOB input = {4, (BYTE *)"test"}, encoded = {0}, decoded = {0};
	if (!CryptProtectData(&input, L"parity", NULL, NULL, NULL, 0, &encoded) || !encoded.pbData || !encoded.cbData) return 16;
	if (!CryptUnprotectData(&encoded, NULL, NULL, NULL, NULL, 0, &decoded) || decoded.cbData != 4) return 17;
	for (DWORD index = 0; index < 4; ++index)
		if (decoded.pbData[index] != input.pbData[index]) return 18;
	LocalFree(encoded.pbData); LocalFree(decoded.pbData);
	BSTR text = SysAllocStringLen(L"parity", 6);
	if (!text || SysStringLen(text) != 6 || lstrcmpW(text, L"parity")) return 19;
	SysFreeString(text);
	HMODULE controls = LoadLibraryW(L"comctl32.dll");
	HIMAGELIST (WINAPI *create)(int, int, UINT, int, int) = (void *)GetProcAddress(controls, "ImageList_Create");
	BOOL (WINAPI *destroy)(HIMAGELIST) = (void *)GetProcAddress(controls, "ImageList_Destroy");
	int (WINAPI *count)(HIMAGELIST) = (void *)GetProcAddress(controls, "ImageList_GetImageCount");
	if (!create || !destroy || !count) return 20;
	HIMAGELIST images = create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
	if (!images || count(images) != 0 || !destroy(images)) return 21;
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	char *(__cdecl *copy_text)(char *, const char *, size_t) = (void *)GetProcAddress(nt, "strncpy");
	format_text = (void *)GetProcAddress(nt, "_vsnprintf");
	char formatted[32], copied[6] = {1, 1, 1, 1, 1, 42};
	if (!copy_text || !format_text || copy_text(copied, "hi", 5) != copied || copied[0] != 'h' || copied[1] != 'i' ||
		copied[2] || copied[3] || copied[4] || copied[5] != 42 || format(formatted, sizeof formatted, "%s %d", "test", 42) != 7 ||
		lstrcmpA(formatted, "test 42")) return 32;
	HMODULE base = LoadLibraryW(L"kernelbase.dll");
	HRESULT (WINAPI *combine_path)(WCHAR *, SIZE_T, const WCHAR *, const WCHAR *, ULONG) = (void *)GetProcAddress(base, "PathCchCombineEx");
	HRESULT (WINAPI *skip_root)(const WCHAR *, const WCHAR **) = (void *)GetProcAddress(base, "PathCchSkipRoot");
	DWORD (WINAPI *current_id)(void) = (void *)GetProcAddress(base, "GetCurrentProcessId");
	struct { const WCHAR *tail; DWORD guard; } root = {NULL, 0xabcdef12};
	if (!base || !combine_path || !skip_root || !current_id || current_id() != GetCurrentProcessId()) return 30;
	if (combine_path(folder, MAX_PATH, L"C:\\Windows", L"..\\Program Files", 0) || lstrcmpW(folder, L"C:\\Program Files") ||
		skip_root(folder, &root.tail) || root.tail != folder + 3 || root.guard != 0xabcdef12) return 31;
	HMODULE shlwapi = LoadLibraryW(L"shlwapi.dll"), theme = LoadLibraryW(L"uxtheme.dll"), script = LoadLibraryW(L"usp10.dll");
	LPWSTR (WINAPI *format_size)(LONGLONG, LPWSTR, UINT) = (void *)GetProcAddress(shlwapi, "StrFormatByteSizeW");
	HRESULT (WINAPI *hit_test)(HANDLE, HDC, int, int, DWORD, const RECT *, HRGN, POINT, WORD *) =
		(void *)GetProcAddress(theme, "HitTestThemeBackground");
	HRESULT (WINAPI *cp_to_x)(void *, int, BOOL, int *) = (void *)GetProcAddress(script, "ScriptStringCPtoX");
	HRESULT (WINAPI *x_to_cp)(void *, int, int *, int *) = (void *)GetProcAddress(script, "ScriptStringXtoCP");
	if (!format_size || !hit_test || !cp_to_x || !x_to_cp || format_size(0x100000000LL, folder, MAX_PATH) != folder ||
		lstrcmpW(folder, L"4.00 GB")) return 23;
	RECT bounds = {10, 10, 20, 20};
	POINT point = {15, 15};
	struct { WORD code, guard; } hit = {0, 0x1234};
	if (hit_test(NULL, NULL, 0, 0, 0, &bounds, NULL, point, &hit.code) || hit.code != 1 || hit.guard != 0x1234) return 24;
	point.x = point.y = -1;
	if (hit_test(NULL, NULL, 0, 0, 0, &bounds, NULL, point, &hit.code) || hit.code || hit.guard != 0x1234) return 25;
	int x = 123, character = 123, trailing = 123;
	if (cp_to_x(NULL, 0, FALSE, &x) >= 0 || x_to_cp(NULL, 0, &character, &trailing) >= 0) return 26;
	cp_to_x = (void *)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "ScriptStringCPtoX");
	x_to_cp = (void *)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "ScriptStringXtoCP");
	if (!cp_to_x || !x_to_cp || cp_to_x(NULL, 0, FALSE, &x) >= 0 || x_to_cp(NULL, 0, &character, &trailing) >= 0) return 29;
	WNDCLASSW definition = {0};
	definition.lpfnWndProc = window_proc;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"RavenParity";
	if (!RegisterClassW(&definition)) return 27;
	HWND parent = CreateWindowExW(0, L"RavenParity", L"", 0, 0, 0, 200, 200, NULL, NULL, definition.hInstance, NULL);
	HWND child = CreateWindowExW(0, L"RavenParity", L"", WS_CHILD, 20, 30, 40, 50, parent, NULL, definition.hInstance, NULL);
	point.x = 25; point.y = 35;
	if (!parent || !child || ChildWindowFromPoint(parent, point) != child || ChildWindowFromPointEx(parent, point, 0) != child) return 28;
	DestroyWindow(parent);
	return 0;
}
