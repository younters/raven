#include <windows.h>

#ifdef RAVEN_LOCAL_LIBRARY
__declspec(dllexport) unsigned RavenLocalMarker(void) { return 73; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#else
__declspec(dllimport) unsigned RavenLocalMarker(void);

int raven_entry(void)
{
	if (RavenLocalMarker() != 73)
		return 1;
	HMODULE local = GetModuleHandleW(L"winhttp.dll");
	if (!local || LoadLibraryW(L"winhttp.dll") != local || !GetProcAddress(local, "RavenLocalMarker"))
		return 2;
	HMODULE system = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!system || system == local || GetProcAddress(system, "RavenLocalMarker") || !GetProcAddress(system, "WinHttpOpen"))
		return 3;
	if (LoadLibraryW(L"C:\\Windows\\System32\\winhttp.dll") != system ||
		LoadLibraryW(L"C:/Windows/System32/winhttp.dll") != system)
		return 4;
	if (LoadLibraryW(L"winhttp.dll") != local || RavenLocalMarker() != 73)
		return 5;
	return 0;
}
#endif
