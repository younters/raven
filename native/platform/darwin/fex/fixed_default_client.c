// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

int raven_entry(void)
{
#ifdef _WIN64
    const uintptr_t image = 0x140000000ULL, allocation = 0x148000000ULL; // inside the 1 GiB image pool, below the shared cache
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
    if (!VirtualFree((void *)allocation, 0, MEM_RELEASE)) return 6;
    for (unsigned round=0; round<4; ++round) {
        for (unsigned slice=0; slice<4; ++slice) {
            uintptr_t at=allocation+slice*0x200000;
            p=VirtualAlloc((void *)at, 0x100000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
            if ((uintptr_t)p!=at) return 7;
            for (unsigned i=0;i<0x100000/sizeof(*p);i+=4096/sizeof(*p)) {
                if (p[i]) return 8;
                p[i]=0x12345678;
            }
        }
        for (unsigned slice=0; slice<4; ++slice)
            if (!VirtualFree((void *)(allocation+slice*0x200000),0,MEM_RELEASE)) return 9;
    }
    return 0;
}
