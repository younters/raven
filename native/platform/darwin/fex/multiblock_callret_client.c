// SPDX-License-Identifier: MIT
// Call-return into invalidated code. X (page 0) jumps directly to F on its own
// 16 KiB page. F calls G indirectly and then returns an immediate. Thread A
// runs X first so X->F is compiled and linked; thread B then enters F through
// that link and its G rewrites F's immediate. The return must see the new code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define F_OFFSET 0x8000
static unsigned char *code;
static void (*volatile g_target)(void);
static volatile DWORD *immediate;

static void noop(void) {}
static void rewrite(void)
{
    DWORD old;
    if (!VirtualProtect(code + F_OFFSET, 0x4000, PAGE_READWRITE, &old)) return;
    *immediate = 2;
    VirtualProtect(code + F_OFFSET, 0x4000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code + F_OFFSET, 0x4000);
}

static DWORD WINAPI run(void *unused)
{
    (void)unused;
    return ((DWORD (*)(void))code)();
}

static DWORD on_thread(void)
{
    DWORD result = 0;
    HANDLE thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (!thread || WaitForSingleObject(thread, 20000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &result)) return 0xff;
    CloseHandle(thread);
    return result;
}

int raven_entry(void)
{
    code = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code) return 1;
    unsigned char *x = code, *p = code + F_OFFSET;
    x[0] = 0xe9; // jmp rel32 F: outside X's multiblock range, so it links
    *(INT32 *)(x + 1) = (INT32)(p - (x + 5));
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xec; *p++ = 0x28;        // sub rsp, 40
    *p++ = 0x48; *p++ = 0xb8;                                   // mov rax, &g_target
    *(void **)p = (void *)&g_target; p += 8;
    *p++ = 0xff; *p++ = 0x10;                                   // call [rax]
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xc4; *p++ = 0x28;        // add rsp, 40
    *p++ = 0xb8; immediate = (volatile DWORD *)p;              // mov eax, imm32
    *(DWORD *)p = 1; p += 4;
    *p++ = 0xc3;                                                // ret
    DWORD old;
    if (!VirtualProtect(code, 0x10000, PAGE_EXECUTE_READ, &old)) return 2;
    FlushInstructionCache(GetCurrentProcess(), code, 0x10000);

    g_target = noop;
    const DWORD first = on_thread();     // A: compiles X and F, links X->F
    if (first != 1) return 10 + (first & 0xf);
    g_target = rewrite;
    const DWORD second = on_thread();    // B: G rewrites F, then returns into it
    g_target = noop;
    const DWORD third = on_thread();     // C: fresh thread sees the new code
    if (third != 2) return 30 + (third & 0xf);
    return second == 2 ? 0 : 20 + (second & 0xf);
}
