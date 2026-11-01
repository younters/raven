// SPDX-License-Identifier: MIT
// Call-return into code another thread invalidated. X (page 0) jumps directly
// to F on its own 16 KiB page. F calls G indirectly and then returns an
// immediate. Thread A runs X first so X->F is compiled and linked, and stays
// alive so B gets its own FEX worker. B enters F through that link, so its own
// lookup cache never holds F. B's G spins without a syscall (a syscall can
// resume B on another worker) while the main thread rewrites F's immediate.
// B's return must then see the new code, not its call-ret stack's stale entry.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define F_OFFSET 0x8000
static unsigned char *code;
static void (*volatile g_target)(void);
static volatile DWORD *immediate;
static volatile LONG spinning, rewritten, finished;

static void noop(void) {}
static void spin(void)
{
    spinning = 1;
    while (!rewritten) {}
}

static DWORD WINAPI warm(void *unused)
{
    (void)unused;
    const DWORD result = ((DWORD (*)(void))code)();
    while (!finished) Sleep(1);
    return result;
}

static DWORD WINAPI run(void *unused)
{
    (void)unused;
    return ((DWORD (*)(void))code)();
}

static int finish(HANDLE thread, DWORD *result)
{
    const int ok = WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, result);
    CloseHandle(thread);
    return ok;
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
    DWORD old, first = 0, second = 0, third = 0;
    if (!VirtualProtect(code, 0x10000, PAGE_EXECUTE_READ, &old)) return 2;
    FlushInstructionCache(GetCurrentProcess(), code, 0x10000);

    g_target = noop;
    HANDLE a = CreateThread(NULL, 0, warm, NULL, 0, NULL); // compiles X and F, links X->F
    if (!a) return 3;
    Sleep(200); // let A return from X before G changes
    g_target = spin;
    HANDLE b = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (!b) return 4;
    for (int i = 0; !spinning; ++i) {
        if (i == 20000) return 5;
        Sleep(1);
    }
    if (!VirtualProtect(code + F_OFFSET, 0x4000, PAGE_READWRITE, &old)) return 6;
    *immediate = 2;
    VirtualProtect(code + F_OFFSET, 0x4000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code + F_OFFSET, 0x4000);
    g_target = noop;
    rewritten = 1;
    if (!finish(b, &second)) return 7;
    finished = 1;
    if (!finish(a, &first) || first != 1) return 10 + (first & 0xf);
    HANDLE c = CreateThread(NULL, 0, run, NULL, 0, NULL); // a fresh thread sees the new code
    if (!c || !finish(c, &third) || third != 2) return 30 + (third & 0xf);
    return second == 2 ? 0 : 20 + (second & 0xf);
}
