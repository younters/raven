#include <windows.h>
#include <stdint.h>

// Exact PC=53 arithmetic, including a transcendental with an exact result.
// Every iteration produces sqrt(3*3+7)+sin(0) == 4.0. Keep the entire
// loop in guest code so this measures Rosetta/x87 rather than API dispatch.
// Accumulate all 2,000,000 results: the expected checksum is 8,000,000.0.
void raven_entry(void)
{
#ifdef RAVEN_X87_CHILD_LAUNCHER
    const char *command = GetCommandLineA();
    int child = 0;
    for (const char *p = command; *p; ++p)
        if (p[0] == ' ' && p[1] == '-' && p[2] == 'c') child = 1;
    if (!child) {
        char module[MAX_PATH];
        char arguments[] = "x87 -child";
        STARTUPINFOA startup = {0};
        PROCESS_INFORMATION process = {0};
        startup.cb = sizeof startup;
        if (!GetModuleFileNameA(NULL, module, sizeof module) ||
            !CreateProcessA(module, arguments, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
            ExitProcess(2);
        if (WaitForSingleObject(process.hProcess, 15000) != WAIT_OBJECT_0)
            ExitProcess(3);
        DWORD code = 4;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ExitProcess(code);
    }
#endif
    const double three = 3.0, seven = 7.0;
    const unsigned short cw = 0x027f;
    union { double value; uint64_t bits; } output;
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    __asm__ volatile (
        "fldcw %3\n\t"
        "fldz\n\t"
        "movl $2000000, %%ecx\n"
        "1: fldl %1\n\t"
        "fmull %1\n\t"
        "faddl %2\n\t"
        "fsqrt\n\t"
        "fldz\n\t"
        "fsin\n\t"
        "faddp\n\t"
        "faddp\n\t"
        "decl %%ecx\n\t"
        "jnz 1b\n\t"
        "fstpl %0"
        : "=m"(output.value) : "m"(three), "m"(seven), "m"(cw) : "ecx", "st", "memory", "cc");
    QueryPerformanceCounter(&end);
    // Fixed little-endian wire record, no CRT or formatting overhead.
    uint64_t report[] = {output.bits, (uint64_t)(end.QuadPart-start.QuadPart), (uint64_t)frequency.QuadPart};
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), report, sizeof report, &written, NULL);
    ExitProcess(output.bits == UINT64_C(0x415e848000000000) && written == sizeof report ? 0 : 1);
}
