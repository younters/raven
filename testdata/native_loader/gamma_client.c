#include <windows.h>

UINT raven_entry(void)
{
	HMODULE crt = LoadLibraryA("ucrtbase.dll");
	double (__cdecl *gamma)(double) = (void *)GetProcAddress(crt, "lgamma");
	float (__cdecl *gamma_float)(float) = (void *)GetProcAddress(crt, "lgammaf");
	/* Windows long double has the double ABI, including on x86. */
	double (__cdecl *gamma_long)(double) = (void *)GetProcAddress(crt, "lgammal");
	if (!crt || !gamma || !gamma_float || !gamma_long) return 1;
	if (gamma(1) != 0 || gamma(2) != 0) return 2;
	if (gamma(5) < 3.1780538303 || gamma(5) > 3.1780538304) return 3;
	if (gamma(-0.5) < 1.2655121234 || gamma(-0.5) > 1.2655121235) return 4;
	if (gamma_float(5) < 3.17805f || gamma_float(5) > 3.17806f) return 5;
	if (gamma_long(5) != gamma(5) || gamma_long(-0.5) != gamma(-0.5)) return 6;
	if (gamma(0) < 1e300 || gamma_float(0) < 1e30f || gamma_long(0) < 1e300) return 7;
	return 0;
}
