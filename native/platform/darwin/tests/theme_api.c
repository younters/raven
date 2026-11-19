#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

int raven_entry(void)
{
	HTHEME theme = OpenThemeData(NULL, L"Progress");
	if (!theme) return 1;
	// Steam's reported stop was at the RET after this provider's INT3.
	// Exercise successful zero HRESULTs repeatedly, checking both guest writes
	// and continuation into the next API call.
	for (int i = 0; i < 32; ++i) {
		int value = -1;
		if (GetThemeInt(theme, 0, 0, TMT_PROGRESSCHUNKSIZE, &value) != S_OK || value != 1) return 2;
		if (GetThemeInt(theme, 0, 0, TMT_PROGRESSSPACESIZE, &value) != S_OK || value != 0) return 3;
	}
	if (SUCCEEDED(GetThemeInt(NULL, 0, 0, TMT_PROGRESSCHUNKSIZE, NULL))) return 4;
	return CloseThemeData(theme) == S_OK ? 0 : 5;
}
