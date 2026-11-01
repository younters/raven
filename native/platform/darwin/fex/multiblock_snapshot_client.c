// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static volatile LONG ready, finish;
static DWORD WINAPI worker(void *unused)
{
    unsigned result;
    __asm__ volatile(
        "mov %%rsp, %%r11\n"
        "lea ready(%%rip), %%r9\n"
        "lea finish(%%rip), %%r10\n"
        "movl $1, (%%r9)\n"
        "1: mov (%%r9), %%eax\n"
        "add $3, %%eax\n"
        "rol $5, %%eax\n"
        "xor $0x678bffff, %%eax\n"
        "cmp %%rsp, %%r11\n"
        "jne 3f\n"
        "mov (%%r10), %%ecx\n"
        "test %%ecx, %%ecx\n"
        "jz 1b\n"
        "xor %%eax, %%eax\n"
        "jmp 4f\n"
        "3: mov $17, %%eax\n4:\n"
        : "=a"(result) : : "rcx", "r9", "r10", "r11", "cc", "memory");
    return result;
}
int raven_entry(void)
{
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
    if (!thread) return 1;
    while (!ready) Sleep(1);
    CONTEXT c={0}; c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    if (SuspendThread(thread)!=0 || !GetThreadContext(thread,&c)) return 2;
    if (c.R9!=(DWORD64)&ready || c.R10!=(DWORD64)&finish || c.R11!=c.Rsp) return 3;
    if (!SetThreadContext(thread,&c)) return 4;
    finish=1;
    if (ResumeThread(thread)!=1 || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0) return 5;
    DWORD code=99;
    if (!GetExitCodeThread(thread,&code)) return 6;
    CloseHandle(thread);
    return code;
}
