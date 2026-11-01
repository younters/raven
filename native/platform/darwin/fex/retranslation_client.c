// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
int raven_entry(void)
{
    BYTE *code = VirtualAlloc(NULL, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    DWORD old, written;
    if (!code) return 1;
    code[0] = 0xb8; code[1] = 42; code[2] = code[3] = code[4] = 0; code[5] = 0xc3;
    char line[] = "RETRANSLATION_CODE=0000000000000000\n";
    uint64_t address = (ULONG_PTR)code;
    for (unsigned i = 0; i < 16; ++i) line[19 + i] = "0123456789abcdef"[(address >> ((15 - i) * 4)) & 15];
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, sizeof(line) - 1, &written, NULL)) return 2;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) return 3;
    if (((unsigned (*)(void))code)() != 42) return 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (!VirtualProtect(code + 4096, 4096, PAGE_READONLY, &old) ||
            !VirtualProtect(code + 4096, 4096, PAGE_READWRITE, &old)) return 5;
        code[4096] = (BYTE)i;
        if (!FlushInstructionCache(GetCurrentProcess(), code + 4096, 4096)) return 6;
        if (((unsigned (*)(void))code)() != 42) return 7;
        if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) return 8;
        if (((unsigned (*)(void))code)() != 42) return 9;
    }
    if (!VirtualProtect(code, 4096, PAGE_READWRITE, &old)) return 10;
    code[1] = 43;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), code, 6)) return 11;
    if (((unsigned (*)(void))code)() != 43) return 12;
    return VirtualFree(code, 0, MEM_RELEASE) ? 0 : 13;
}
