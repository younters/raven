// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD WINAPI run(void *arg)
{
	unsigned value = (unsigned)(ULONG_PTR)arg, actual;
	// No ABI call in this interval: every bit of YMM0 is live across scheduler
	// preemption, including upper halves that Win64 function calls may clobber.
	__asm__ volatile(
		"vmovd %1, %%xmm0\n"
		"vinsertf128 $1, %%xmm0, %%ymm0, %%ymm0\n"
		"mov $100000000, %%ecx\n"
		"1: dec %%ecx\njnz 1b\n"
		"vextractf128 $1, %%ymm0, %%xmm1\n"
		"vmovd %%xmm1, %0\nvzeroupper"
		: "=r"(actual) : "r"(value) : "rcx", "xmm0", "xmm1", "cc");
	return actual == value ? 0 : 1;
}
int raven_entry(void)
{
	unsigned eax = 1, ebx, ecx = 0, edx;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
	if (!(ecx & (1u << 28))) return 77;
	HANDLE worker = CreateThread(NULL, 0, run, (void *)123, 0, NULL);
	if (!worker) return 10;
	DWORD result = run((void *)456), other = 99;
	if (WaitForSingleObject(worker, 30000) != WAIT_OBJECT_0) return 11;
	if (!GetExitCodeThread(worker, &other)) return 12;
	CloseHandle(worker);
	return result ? result : other;
}
