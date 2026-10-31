// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

int raven_entry(void)
{
#ifdef _WIN64
    const uintptr_t image = 0x140000000ULL, allocation = 0x190000000ULL;
#else
    const uintptr_t image = 0x400000, allocation = 0x600000;
#endif
    if ((uintptr_t)GetModuleHandleW(NULL) != image) return 1;
    // Exercise ownership, overlap rejection, and restoration of a pool page.
    volatile unsigned *p = VirtualAlloc((void *)allocation, 65536,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uintptr_t)p != allocation) return 2;
    *p = 0x12345678;
    if (VirtualAlloc((void *)allocation, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 3;
    if (*p != 0x12345678 || !VirtualFree((void *)allocation, 0, MEM_RELEASE)) return 4;
    p = VirtualAlloc((void *)allocation, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uintptr_t)p != allocation || *p) return 5;
    return VirtualFree((void *)allocation, 0, MEM_RELEASE) ? 0 : 6;
}
