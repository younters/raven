#include <windows.h>

int raven_entry(void)
{
    HMODULE crt = LoadLibraryA("api-ms-win-crt-math-l1-1-0.dll");
    if (!crt) return 1;
    long long (__cdecl *round_float)(float) = (void *)GetProcAddress(crt, "llrintf");
    if (!round_float) return 2;
    if (round_float(1.5f) != 2 || round_float(2.5f) != 2 ||
        round_float(-1.5f) != -2 || round_float(-2.5f) != -2) return 3;
    if (round_float(0.0f) != 0 || round_float(-0.0f) != 0) return 4;
    if (round_float(4294967296.0f) != 4294967296LL ||
        round_float(-4294967296.0f) != -4294967296LL) return 5;
    return 0;
}
