#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <shlobj.h>

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
	return 0;
}
