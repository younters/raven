// A library loaded by its path with LOAD_WITH_ALTERED_SEARCH_PATH finds the libraries it imports in its own folder: altered\root.dll
// imports altered\dependency.dll, which is not beside the program.
#include <windows.h>

#if defined(RAVEN_ALTERED_DEPENDENCY)
__declspec(dllexport) unsigned RavenAlteredDependency(void) { return 41; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#elif defined(RAVEN_ALTERED_ROOT)
__declspec(dllimport) unsigned RavenAlteredDependency(void);
__declspec(dllexport) unsigned RavenAlteredRoot(void) { return RavenAlteredDependency() + 1; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#else
typedef unsigned (*root_function)(void);

int raven_entry(void)
{
	WCHAR path[MAX_PATH + 32];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return 1;
	while (length && path[length - 1] != L'\\')
		--length;
	const WCHAR tail[] = L"altered\\root.dll";
	for (DWORD index = 0; index < sizeof(tail) / sizeof(tail[0]); ++index)
		path[length + index] = tail[index];
	HMODULE root = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!root)
		return 3;
	const root_function function = (root_function)GetProcAddress(root, "RavenAlteredRoot");
	if (!function || function() != 42)
		return 4;
	if (!GetModuleHandleW(L"dependency.dll"))
		return 5;
	return 0;
}
#endif
