// SPDX-License-Identifier: MIT
// Multiblock refcount/ordering stress. Workers run one guest function whose
// loop has internal branches, LOCK XADD reference counts and plain
// publish/consume stores. The controller repeatedly suspends and inspects the
// workers, forcing cooperative stops inside the compiled loop. Any replayed
// or skipped guest instruction breaks the exact counter totals.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WORKERS 4
#define ITERATIONS 2000000

struct shared {
    volatile LONG refs, odd;
    volatile LONG64 slot[WORKERS];
    volatile LONG64 bad;
    volatile LONG start;
};
static struct shared shared;
static volatile LONG done[WORKERS];
static LONG64 result_local[WORKERS], result_odd[WORKERS];

static DWORD WINAPI worker(void *argument)
{
    LONG64 index = (LONG64)argument, local, odd_local;
    volatile LONG64 *slot = &shared.slot[index];
    while (!shared.start) {}
    __asm__ volatile(
        "mov $" "2000000" ", %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "1:\n"
        "mov $1, %%eax\n"
        "lock xaddl %%eax, (%[refs])\n"
        "inc %%r9\n"
        "test $1, %%r9b\n"
        "jz 2f\n"
        "inc %%r10\n"
        "mov $1, %%edx\n"
        "lock xaddl %%edx, (%[odd])\n"
        "mov %%r9, (%[slot])\n"
        "mov (%[slot]), %%r11\n"
        "cmp %%r11, %%r9\n"
        "je 2f\n"
        "lock incq (%[bad])\n"
        "2:\n"
        "mov $-1, %%eax\n"
        "lock xaddl %%eax, (%[refs])\n"
        "mov $1, %%eax\n"
        "lock xaddl %%eax, (%[refs])\n"
        // FEX folds this compare into the branch when both successors
        // overwrite flags inside the multiblock; NZCV is then never written.
        "sub $1, %%r8\n"
        "cmp $0, %%r8\n"
        "jne 1b\n"
        "add $0, %%r10\n"
        "mov %%r9, %[local]\n"
        "mov %%r10, %[oddl]\n"
        : [local] "=m"(local), [oddl] "=m"(odd_local)
        : [refs] "r"(&shared.refs), [odd] "r"(&shared.odd), [slot] "r"(slot), [bad] "r"(&shared.bad)
        : "rax", "rdx", "r8", "r9", "r10", "r11", "cc", "memory");
    result_local[index] = local;
    result_odd[index] = odd_local;
    done[index] = 1;
    return 0;
}

int raven_entry(void)
{
    HANDLE threads[WORKERS];
    for (LONG64 i = 0; i < WORKERS; ++i)
        if (!(threads[i] = CreateThread(NULL, 0, worker, (void *)i, 0, NULL))) return 1;
    shared.start = 1;
    unsigned suspensions = 0;
    for (;;) {
        int running = 0;
        for (int i = 0; i < WORKERS; ++i) {
            if (done[i]) continue;
            running = 1;
            CONTEXT c = {0};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(threads[i]) == (DWORD)-1) return 2;
            if (!GetThreadContext(threads[i], &c)) return 3;
            if (ResumeThread(threads[i]) == (DWORD)-1) return 4;
            ++suspensions;
        }
        if (!running) break;
    }
    for (int i = 0; i < WORKERS; ++i) {
        if (WaitForSingleObject(threads[i], 10000) != WAIT_OBJECT_0) return 5;
        if (result_local[i] != ITERATIONS) return 10 + i;
        if (result_odd[i] != ITERATIONS / 2) return 20 + i;
    }
    if (shared.refs != (LONG)WORKERS * ITERATIONS) return 30;
    if (shared.odd != (LONG)WORKERS * ITERATIONS / 2) return 31;
    if (shared.bad) return 32;
    return suspensions ? 0 : 40;
}
