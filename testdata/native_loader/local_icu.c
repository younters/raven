// A program that ships its own icuuc.dll gets that one, with the export names of its own build, whether it imports it or loads it later.
#include <windows.h>

#ifdef RAVEN_LOCAL_ICU
__declspec(dllexport) unsigned u_ravenLocal_99(void) { return 99; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#else
typedef unsigned (*local_function)(void);
#ifndef RAVEN_ICU_LATER
__declspec(dllimport) unsigned u_ravenLocal_99(void);
#endif

int raven_entry(void)
{
#ifndef RAVEN_ICU_LATER
	if (u_ravenLocal_99() != 99)
		return 1;
	if (!GetModuleHandleW(L"icuuc.dll"))
		return 2;
#endif
	HMODULE icu = LoadLibraryW(L"icuuc.dll");
	if (!icu)
		return 3;
	const local_function function = (local_function)GetProcAddress(icu, "u_ravenLocal_99");
	if (!function || function() != 99)
		return 4;
	return 0;
}
#endif
