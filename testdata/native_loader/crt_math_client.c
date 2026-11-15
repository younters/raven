#include <windows.h>

/* C99 math entry points of the Universal CRT, called by name from the math API set. A long double is a double in Microsoft's runtime,
   so the l forms are declared with double here: a form that returned an x87 value on x64, or read a wider argument on x86, fails. */

typedef struct {
	long long quotient, remainder;
} Division;

static int within(double value, double low, double high) { return value >= low && value <= high; }

int raven_entry(void)
{
	HMODULE crt = LoadLibraryA("api-ms-win-crt-math-l1-1-0.dll");
	HMODULE runtime = LoadLibraryA("api-ms-win-crt-utility-l1-1-0.dll");
	if (!crt || !runtime) return 1;

	long long (__cdecl *round_float)(float) = (void *)GetProcAddress(crt, "llrintf");
	if (!round_float) return 2;
	/* The default rounding mode is to nearest, ties to even. */
	if (round_float(1.5f) != 2 || round_float(2.5f) != 2 || round_float(-1.5f) != -2 || round_float(-2.5f) != -2) return 3;
	if (round_float(0.0f) != 0 || round_float(4294967296.0f) != 4294967296LL || round_float(-4294967296.0f) != -4294967296LL) return 4;

	double (__cdecl *gamma)(double) = (void *)GetProcAddress(crt, "lgamma");
	float (__cdecl *gamma_float)(float) = (void *)GetProcAddress(crt, "lgammaf");
	double (__cdecl *gamma_long)(double) = (void *)GetProcAddress(crt, "lgammal");
	if (!gamma || !gamma_float || !gamma_long) return 5;
	if (gamma(1) != 0 || gamma(2) != 0) return 6;
	if (!within(gamma(5), 3.1780538303, 3.1780538304) || !within(gamma(-0.5), 1.2655121234, 1.2655121235)) return 7;
	if (!within(gamma_float(5), 3.17805f, 3.17806f) || gamma_long(5) != gamma(5) || gamma_long(-0.5) != gamma(-0.5)) return 8;
	if (gamma(0) < 1e300 || gamma_long(0) < 1e300) return 9;

	double (__cdecl *arc_sine)(double) = (void *)GetProcAddress(crt, "asinh");
	double (__cdecl *arc_cosine)(double) = (void *)GetProcAddress(crt, "acosh");
	double (__cdecl *arc_tangent)(double) = (void *)GetProcAddress(crt, "atanh");
	if (!arc_sine || !arc_cosine || !arc_tangent) return 10;
	/* log(2e300) = 691.46867507877... */
	if (arc_sine(1e-200) != 1e-200 || arc_sine(-1e-200) != -1e-200) return 11;
	if (!within(arc_sine(1e300), 691.4686, 691.4688) || !within(arc_sine(-1e300), -691.4688, -691.4686)) return 12;
	if (!within(arc_cosine(1e300), 691.4686, 691.4688) || arc_cosine(1) != 0) return 13;
	const double below = arc_cosine(0.5);
	if (below == below) return 14;
	if (arc_tangent(1e-200) != 1e-200 || arc_tangent(-1e-200) != -1e-200) return 15;
	if (!within(arc_tangent(0.5), 0.54930614433405, 0.54930614433406) || !within(arc_tangent(-0.5), -0.54930614433406, -0.54930614433405))
		return 16;

	float (__cdecl *nan_float)(const char *) = (void *)GetProcAddress(crt, "nanf");
	double (__cdecl *nan_long)(const char *) = (void *)GetProcAddress(crt, "nanl");
	double (__cdecl *toward)(double, double) = (void *)GetProcAddress(crt, "nexttoward");
	if (!nan_float || !nan_long || !toward) return 17;
	const float quiet_float = nan_float("");
	const double quiet_long = nan_long("");
	if (quiet_float == quiet_float || quiet_long == quiet_long) return 18;
	if (toward(1, 2) != 1.0000000000000002 || toward(0, -1) >= 0 || toward(1, 1) != 1) return 19;

	Division (__cdecl *divide)(long long, long long) = (void *)GetProcAddress(runtime, "lldiv");
	Division (__cdecl *divide_max)(long long, long long) = (void *)GetProcAddress(runtime, "imaxdiv");
	if (!divide || !divide_max) return 20;
	Division result = divide(-7, 2);
	if (result.quotient != -3 || result.remainder != -1) return 21;
	result = divide_max(12345678901234LL, 1000000);
	if (result.quotient != 12345678 || result.remainder != 901234) return 22;

	/* Existing neighbours, kept working: cbrt, fma, remainder, nextafterf. */
	double (__cdecl *cube)(double) = (void *)GetProcAddress(crt, "cbrt");
	double (__cdecl *fused)(double, double, double) = (void *)GetProcAddress(crt, "fma");
	double (__cdecl *remain)(double, double) = (void *)GetProcAddress(crt, "remainder");
	float (__cdecl *next_float)(float, float) = (void *)GetProcAddress(crt, "nextafterf");
	if (!cube || !fused || !remain || !next_float) return 23;
	if (cube(-8) != -2 || remain(7, 2) != -1 || remain(5, 2) != 1) return 24;
	if (fused(1.0000000000000002, 0.9999999999999998, -1) != -4.930380657631324e-32) return 25;
	if (next_float(1, 2) != 1.00000011920928955078125f) return 26;
	return 0;
}
