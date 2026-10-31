// SPDX-License-Identifier: MIT
#include <windows.h>
#ifndef EXPECT_LAA
#define EXPECT_LAA 0
#endif
static volatile unsigned sentinel = 0x12345678;
int raven_entry(void)
{
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	if ((ULONG_PTR)info.lpMaximumApplicationAddress != (EXPECT_LAA ? 0xfffeffffu : 0x7ffeffffu)) return 1;
	if (sentinel != 0x12345678) return 2;
	if (((ULONG_PTR)GetModuleHandleW(0) >= 0x80000000u) != EXPECT_LAA) return 3;
	void *high = VirtualAlloc((void *)0x90010000u, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if ((high != 0) != EXPECT_LAA) return 4;
	if (high) {
		*(volatile unsigned *)high = 42;
		if (*(volatile unsigned *)high != 42) return 5;
		if (!VirtualFree(high, 0, MEM_RELEASE)) return 6;
	}
	if (VirtualAlloc((void *)0xffff0000u, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 7;
	return 0;
}
