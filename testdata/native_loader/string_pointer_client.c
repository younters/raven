#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* LoadStringW with no capacity stores a read-only pointer to the string, and a 32-bit program's buffer holds only that
 * pointer: the word after it must survive the call. */

int raven_entry(void)
{
	struct {
		const WCHAR *text;
		volatile DWORD guard;
	} slot = {NULL, 0x52415645};
	if (LoadStringW(NULL, 1, (WCHAR *)&slot.text, 0) != 5 || slot.guard != 0x52415645)
		return 1;
	if (!slot.text || slot.text[0] != L'A' || slot.text[4] != L'B')
		return 2;
	return 0;
}
