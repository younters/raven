// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int contains(const char *text, const char *part)
{
	for (; *text; ++text) {
		unsigned i = 0;
		while (part[i] && (text[i] | 32) == part[i]) ++i;
		if (!part[i]) return 1;
	}
	return 0;
}
int raven_entry(void)
{
	char module[MAX_PATH];
#ifndef _WIN64
	const char *folder = "syswow64";
#endif
	HMODULE library = LoadLibraryA("windowscodecs.dll");
	if (!library) return 1;
	if (!GetModuleFileNameA(library, module, MAX_PATH)) return 2;
#ifndef _WIN64
	if (!contains(module, folder)) return 3;
#endif
	return 0;
}
