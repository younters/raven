/* Raven's wrapper-library policy (10515aec): the program's winmm and Raven's
 * system provider coexist; explicit system paths and SEARCH_SYSTEM32 select
 * the provider. This is not a Windows loaded-module reuse conformance test. */
#include <windows.h>

#ifdef RAVEN_LOCAL_LIBRARY
__declspec(dllexport) unsigned RavenLocalMarker(void) { return 73; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance; (void)reason; (void)reserved;
	return TRUE;
}
#else
__declspec(dllimport) unsigned RavenLocalMarker(void);

int raven_entry(void)
{
	if (RavenLocalMarker() != 73)
		return 1;
	HMODULE local = GetModuleHandleW(L"winmm.dll");
	if (!local || LoadLibraryW(L"winmm.dll") != local || !GetProcAddress(local, "RavenLocalMarker"))
		return 2;
	HMODULE system = LoadLibraryExW(L"winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!system || system == local || GetProcAddress(system, "RavenLocalMarker") || !GetProcAddress(system, "timeGetTime"))
		return 3;
	if (LoadLibraryW(L"C:\\Windows\\System32\\winmm.dll") != system ||
		LoadLibraryW(L"C:\\Windows\\SysWOW64\\winmm.dll") != system)
		return 4;
	if (LoadLibraryW(L"winmm.dll") != local || RavenLocalMarker() != 73)
		return 5;
	return 0;
}
#endif
