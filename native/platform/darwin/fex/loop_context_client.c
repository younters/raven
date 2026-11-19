// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Keep pointers, a stack anchor, and vector state live across inner back edges
// while another guest thread forces cooperative scheduler handoffs.
static DWORD WINAPI run(void *unused)
{
	BYTE *data = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!data) return 1;
	unsigned bad;
	__asm__ volatile(
		"mov %[data], %%r8\n"
		"mov %%r8, %%r9\n"
		"lea 65536(%%r8), %%r10\n"
		"mov %%rsp, %%r11\n"
		"mov $128, %%r12d\n"
		"pxor %%xmm1, %%xmm1\n"
		"1: movdqu (%%r9), %%xmm0\n"
		"paddd %%xmm0, %%xmm1\n"
		"pshufd $0x1b, %%xmm1, %%xmm2\n"
		"pxor %%xmm2, %%xmm1\n"
		"add $16, %%r9\n"
		"cmp %%rsp, %%r11\njne 4f\n"
		"cmp %%r10, %%r9\njne 1b\n"
		"mov %%r8, %%r9\n"
		"dec %%r12d\njnz 1b\n"
		"xor %%eax, %%eax\njmp 5f\n"
		"4: mov $2, %%eax\n5:\n"
		: "=a"(bad) : [data] "r"(data)
		: "r8", "r9", "r10", "r11", "r12", "xmm0", "xmm1", "xmm2", "cc", "memory");
	VirtualFree(data, 0, MEM_RELEASE);
	return bad;
}
int raven_entry(void)
{
	HANDLE worker = CreateThread(NULL, 0, run, NULL, 0, NULL);
	if (!worker) return 10;
	DWORD result = run(NULL), other = 99;
	if (WaitForSingleObject(worker, 30000) != WAIT_OBJECT_0) return 11;
	if (!GetExitCodeThread(worker, &other)) return 12;
	CloseHandle(worker);
	return result ? result : other;
}
