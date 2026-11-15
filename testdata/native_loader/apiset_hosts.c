// The process-status and version API sets name the libraries that export their functions: K32EnumProcesses comes from kernel32.dll, as
// on Windows (psapi.dll exports it without the prefix), and the version functions from version.dll.
#include <windows.h>

typedef BOOL (WINAPI *enum_processes_function)(DWORD *, DWORD, DWORD *);

int raven_entry(void)
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	HMODULE process_status = LoadLibraryW(L"api-ms-win-core-psapi-l1-1-0.dll");
	if (!kernel32 || !process_status)
		return 1;
	const enum_processes_function enum_processes = (enum_processes_function)GetProcAddress(process_status, "K32EnumProcesses");
	if (!enum_processes || enum_processes != (enum_processes_function)GetProcAddress(kernel32, "K32EnumProcesses"))
		return 2;
	DWORD processes[64], bytes = 0;
	if (!enum_processes(processes, sizeof(processes), &bytes) || !bytes)
		return 3;
	HMODULE version = LoadLibraryW(L"version.dll");
	HMODULE version_set = LoadLibraryW(L"api-ms-win-core-version-l1-1-0.dll");
	if (!version || !version_set)
		return 4;
	static const char *const names[] = {"GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerQueryValueW"};
	for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		FARPROC function = GetProcAddress(version_set, names[index]);
		if (!function || function != GetProcAddress(version, names[index]))
			return 5 + index;
	}
	return 0;
}
