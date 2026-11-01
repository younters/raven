// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

extern void probe(void), fault_pc(void), resume_pc(void);
static volatile LONG seen;

// Both flag-producing instructions share one compilation region. The compare's
// value and flags are dead, but its access violation is observable by Windows.
__asm__(".text\n"
        ".globl probe\nprobe:\n"
        "xor %eax,%eax\n"
        ".globl fault_pc\nfault_pc:\n"
        "cmpq (%rax),%rax\n"
        ".globl resume_pc\nresume_pc:\n"
        "xor %eax,%eax\n"
        "ret\n");

static LONG CALLBACK exception(EXCEPTION_POINTERS *p)
{
    if (p->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        p->ContextRecord->Rip != (uintptr_t)fault_pc ||
        p->ExceptionRecord->ExceptionInformation[1] != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    ++seen;
    p->ContextRecord->Rip = (uintptr_t)resume_pc;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int raven_entry(void)
{
    void *handler = AddVectoredExceptionHandler(1, exception);
    if (!handler) return 2;
    probe();
    if (!RemoveVectoredExceptionHandler(handler)) return 3;
    return seen == 1 ? 0 : 1;
}
