// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

int raven_entry(void);
// A real absolute relocation: this initialized pointer must follow the image.
static void *volatile relocated_entry = (void *)&raven_entry;

int raven_entry(void)
{
	uintptr_t teb;
	__asm__("movq %%gs:0x30,%0" : "=r"(teb));
	if (!teb || *(uintptr_t *)(teb + 0x30) != teb) return 1;
	uintptr_t peb = *(uintptr_t *)(teb + 0x60);
	if (!peb || !*(uintptr_t *)(peb + 0x10)) return 2;
	if (relocated_entry != (void *)&raven_entry) return 3;
	if ((uintptr_t)GetModuleHandleW(NULL) != *(uintptr_t *)(peb + 0x10)) return 4;
	if (*(DWORD *)(teb + 0x48) != GetCurrentThreadId()) return 5;
	SetLastError(0x1234);
	if (GetLastError() != 0x1234) return 6;
	// Under Tier 0 this low address is repaired through FEX's shared-data decoder.
	if (!*(volatile DWORD *)(uintptr_t)0x7ffe026c) return 7;
	return 0;
}
