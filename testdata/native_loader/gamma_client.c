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
	double (__cdecl *cube)(double) = (void *)GetProcAddress(crt, "cbrt");
	double (__cdecl *fused)(double, double, double) = (void *)GetProcAddress(crt, "fma");
	double (__cdecl *remain)(double, double) = (void *)GetProcAddress(crt, "remainder");
	double (__cdecl *next)(double, double) = (void *)GetProcAddress(crt, "nexttoward");
	float (__cdecl *next_float)(float, float) = (void *)GetProcAddress(crt, "nextafterf");
	int (__cdecl *rounding)(void) = (void *)GetProcAddress(crt, "fegetround");
	double (__cdecl *inverse)(double) = (void *)GetProcAddress(crt, "asinh");
	if (!cube || !fused || !remain || !next || !next_float || !rounding || !inverse) return 8;
	if (cube(-8) != -2 || remain(7, 2) != -1 || remain(5, 2) != 1) return 9;
	if (fused(1.0000000000000002, 0.9999999999999998, -1) != -4.930380657631324e-32) return 10;
	if (next(1, 2) != 1.0000000000000002 || next(0, -1) >= 0 || next_float(1, 2) != 1.00000011920928955078125f) return 11;
	if (rounding() != 0 || inverse(1e-200) != 1e-200 || inverse(1e300) < 690 || inverse(-1e300) > -690) return 12;
	int (__cdecl *temporary)(WCHAR *, size_t) = (void *)GetProcAddress(crt, "_wmktemp_s");
	int (__cdecl *convert)(long long, WCHAR *, size_t, int) = (void *)GetProcAddress(crt, "_i64tow_s");
	WCHAR name[] = L"unicode-\u00e9-XXXXXX", text[24];
	if (!temporary || !convert || temporary(name, sizeof(name) / sizeof(*name)) || name[8] != 0xe9 || name[10] == 'X') return 13;
	if (convert((-9223372036854775807LL - 1), text, 24, 10) || text[0] != '-' || text[19] != '8' || text[20]) return 14;
	if (convert(123, text, 2, 10) != 34 || text[0] || temporary(text, 0) != 22) return 15;
	return 0;
}
