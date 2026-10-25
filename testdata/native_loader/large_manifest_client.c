#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) int private_value(void);

/* A self-extracting installer has a small manifest beside a large resource payload. Its supportedOS declaration still applies. */
int raven_entry(void)
{
	if (private_value() != 42) return 39;
	typedef BOOL (WINAPI *wait_address)(volatile void *, void *, SIZE_T, DWORD);
	wait_address wait = (wait_address)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitOnAddress");
	DWORD address = 1, expected = 0;
	if (!wait || !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WakeByAddressSingle") ||
		!GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WakeByAddressAll") || !wait(&address, &expected, sizeof(address), 0) ||
		!SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1)) return 38;

	OSVERSIONINFOEXW version = {sizeof(version)};
	if (!GetVersionExW((OSVERSIONINFOW *)&version) || version.dwMajorVersion != 10 || version.dwMinorVersion ||
		version.dwBuildNumber != 19045 || version.wProductType != VER_NT_WORKSTATION) return 30;
	typedef LONG (WINAPI *get_version)(OSVERSIONINFOW *);
	get_version rtl = (get_version)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (!rtl || rtl((OSVERSIONINFOW *)&version) || version.dwMajorVersion != 10 || version.dwMinorVersion || version.dwBuildNumber != 19045) return 31;
	const volatile BYTE *shared = (const volatile BYTE *)0x7ffe0000;
	if (*(const volatile DWORD *)(shared + 0x260) != 19045 || *(const volatile DWORD *)(shared + 0x26c) != 10 ||
		*(const volatile DWORD *)(shared + 0x270)) return 32;
	HKEY key;
	DWORD value, size = sizeof(value);
	WCHAR text[32];
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key)) return 33;
	if (RegQueryValueExW(key, L"CurrentMajorVersionNumber", NULL, NULL, (BYTE *)&value, &size) || value != 10) return 34;
	size = sizeof(value);
	if (RegQueryValueExW(key, L"UBR", NULL, NULL, (BYTE *)&value, &size) || value != 2364) return 35;
	size = sizeof(text);
	if (RegQueryValueExW(key, L"CurrentBuildNumber", NULL, NULL, (BYTE *)text, &size) || lstrcmpW(text, L"19045")) return 36;
	size = sizeof(text);
	/* Windows 10 keeps the legacy CurrentVersion string at 6.3; the modern DWORDs and build number identify it. */
	if (RegQueryValueExW(key, L"CurrentVersion", NULL, NULL, (BYTE *)text, &size) || lstrcmpW(text, L"6.3")) return 37;
	RegCloseKey(key);
	return 0;
}

