// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD WINAPI run(void *arg)
{
    unsigned long long expected[64] __attribute__((aligned(32))), actual[64];
    for (unsigned i=0;i<64;++i) expected[i]=((unsigned long long)(ULONG_PTR)arg<<32)|i;
    __asm__ volatile(
        "vmovdqu 0(%%rsi), %%ymm0\n"
        "vmovdqu 32(%%rsi), %%ymm1\n"
        "vmovdqu 64(%%rsi), %%ymm2\n"
        "vmovdqu 96(%%rsi), %%ymm3\n"
        "vmovdqu 128(%%rsi), %%ymm4\n"
        "vmovdqu 160(%%rsi), %%ymm5\n"
        "vmovdqu 192(%%rsi), %%ymm6\n"
        "vmovdqu 224(%%rsi), %%ymm7\n"
        "vmovdqu 256(%%rsi), %%ymm8\n"
        "vmovdqu 288(%%rsi), %%ymm9\n"
        "vmovdqu 320(%%rsi), %%ymm10\n"
        "vmovdqu 352(%%rsi), %%ymm11\n"
        "vmovdqu 384(%%rsi), %%ymm12\n"
        "vmovdqu 416(%%rsi), %%ymm13\n"
        "vmovdqu 448(%%rsi), %%ymm14\n"
        "vmovdqu 480(%%rsi), %%ymm15\n"
        "mov $100000000, %%ecx\n"
        "1: dec %%ecx\njnz 1b\n"
        "vmovdqu %%ymm0, 0(%%rdi)\n"
        "vmovdqu %%ymm1, 32(%%rdi)\n"
        "vmovdqu %%ymm2, 64(%%rdi)\n"
        "vmovdqu %%ymm3, 96(%%rdi)\n"
        "vmovdqu %%ymm4, 128(%%rdi)\n"
        "vmovdqu %%ymm5, 160(%%rdi)\n"
        "vmovdqu %%ymm6, 192(%%rdi)\n"
        "vmovdqu %%ymm7, 224(%%rdi)\n"
        "vmovdqu %%ymm8, 256(%%rdi)\n"
        "vmovdqu %%ymm9, 288(%%rdi)\n"
        "vmovdqu %%ymm10, 320(%%rdi)\n"
        "vmovdqu %%ymm11, 352(%%rdi)\n"
        "vmovdqu %%ymm12, 384(%%rdi)\n"
        "vmovdqu %%ymm13, 416(%%rdi)\n"
        "vmovdqu %%ymm14, 448(%%rdi)\n"
        "vmovdqu %%ymm15, 480(%%rdi)\n"
        "vzeroupper"
        :: "S"(expected), "D"(actual) : "rcx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    for (unsigned i=0;i<64;++i) if (actual[i]!=expected[i]) return i+1;
    return 0;
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
