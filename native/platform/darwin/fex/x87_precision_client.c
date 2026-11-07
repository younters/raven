/* x87 arithmetic in each precision the control word selects, hashed bit for bit, so engines can be compared with each other. Operands are
   built from bit patterns and each operation is written as x87 instructions, so the hashes do not depend on the compiler. They cover what a
   translator could get wrong: values with more bits than the precision, values outside the double range, tiny and huge results, signed
   zeros, infinities and NaNs. Build with RAVEN_X87_PRINT to write the hashes to C:\x87hash.txt for new reference values. */
#include <windows.h>

typedef struct { unsigned long long mantissa; unsigned short top; } extended;
static unsigned long long hashes[3];
static unsigned seed = 12345;

static unsigned next(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }
static void control(unsigned short word) { __asm__ volatile("fldcw %0" : : "m"(word)); }
static void mix(int set, const void *data, int size)
{
	const unsigned char *bytes = data;
	for (int i = 0; i < size; ++i) {
		hashes[set] ^= bytes[i];
		hashes[set] *= 1099511628211ULL;
	}
}

#define COUNT 72
static extended values[COUNT];

/* bits: how many leading mantissa bits may be set (24, 53 or 64); exponent: unbiased. */
static extended make(int negative, int exponent, int bits)
{
	extended value;
	/* One call per statement: C leaves the order of calls within an expression to the compiler. */
	unsigned long long mantissa = (unsigned long long)next() << 40;
	mantissa ^= (unsigned long long)next() << 16;
	mantissa ^= next();
	mantissa |= 1ULL << 63;
	if (bits < 64)
		mantissa &= ~((1ULL << (64 - bits)) - 1);
	value.mantissa = mantissa;
	value.top = (unsigned short)((negative ? 0x8000 : 0) | (exponent + 16383));
	return value;
}

static void operands(void)
{
	int n = 0;
	static const int widths[3] = {24, 53, 64};
	for (int w = 0; w < 3; ++w)
		for (int i = 0; i < 12; ++i) {
			const int negative = next() & 1;
			const int exponent = (int)(next() % 61) - 30;
			values[n++] = make(negative, exponent, widths[w]);
		}
	/* Near and past the edges of the float and double ranges. */
	static const int edges[] = {-126, -127, -149, -1022, -1023, -1074, -1100, 127, 128, 1023, 1024, -16000, 16000};
	for (int i = 0; i < 13; ++i) {
		const int negative = next() & 1;
		values[n++] = make(negative, edges[i], i & 1 ? 53 : 24);
	}
	const extended special[] = {
		{0, 0}, {0, 0x8000},                          /* zeros */
		{1ULL << 63, 0x3fff}, {1ULL << 63, 0xbfff},     /* one and minus one */
		{1ULL << 63, 0x7fff}, {1ULL << 63, 0xffff},     /* infinities */
		{3ULL << 62, 0x7fff},                         /* a quiet NaN */
		{1ULL << 40, 0},                              /* a denormal */
	};
	for (unsigned i = 0; i < sizeof special / sizeof *special; ++i)
		values[n++] = special[i];
	while (n < COUNT) {
		const int negative = next() & 1;
		const int exponent = (int)(next() % 200) - 100;
		values[n++] = make(negative, exponent, 64);
	}
}

static void operate(const extended *a, const extended *b, extended *r)
{
	/* a + b, a - b, a * b, a / b and the square root of a; then the invalid-operation flag. */
	__asm__ volatile("fnclex\n\t"
		"fldt %[b]\n\tfldt %[a]\n\tfadd %%st(1), %%st\n\tfstpt %[r0]\n\t"
		"fldt %[a]\n\tfsub %%st(1), %%st\n\tfstpt %[r1]\n\t"
		"fldt %[a]\n\tfmul %%st(1), %%st\n\tfstpt %[r2]\n\t"
		"fldt %[a]\n\tfdiv %%st(1), %%st\n\tfstpt %[r3]\n\t"
		"fstp %%st(0)\n\tfldt %[a]\n\tfsqrt\n\tfstpt %[r4]\n\t"
		: [r0] "=m"(r[0]), [r1] "=m"(r[1]), [r2] "=m"(r[2]), [r3] "=m"(r[3]), [r4] "=m"(r[4])
		: [a] "m"(*a), [b] "m"(*b));
}

int raven_entry(void)
{
	static const unsigned short words[3] = {0x07f, 0x27f, 0x37f};
	operands();
	for (int set = 0; set < 3; ++set) {
		hashes[set] = 1469598103934665603ULL;
		for (int i = 0; i < COUNT; ++i)
			for (int j = 0; j < COUNT; ++j) {
				extended r[5];
				unsigned short status;
				control(words[set]);
				operate(&values[i], &values[j], r);
				__asm__ volatile("fnstsw %0" : "=m"(status));
				status &= 1;
				for (int k = 0; k < 5; ++k)
					mix(set, &r[k], 10);
				mix(set, &status, 2);
			}
	}
	control(0x27f);
#ifdef RAVEN_X87_PRINT
	HANDLE out = CreateFileA("C:\\x87hash.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	for (int set = 0; set < 3; ++set) {
		char line[18];
		for (int digit = 0; digit < 16; ++digit)
			line[digit] = "0123456789abcdef"[(hashes[set] >> (60 - digit * 4)) & 15];
		line[16] = '\r';
		line[17] = '\n';
		DWORD written;
		WriteFile(out, line, 18, &written, 0);
	}
	CloseHandle(out);
	return 0;
#else
	/* The hashes Rosetta and the software x87 of FEX both give: the returned bits name each precision that differs. */
	const unsigned long long expected[3] = {0xde6338bce2b83789ULL, 0xc355a353b85f3c45ULL, 0x4175ed86616ecc09ULL};
	int failed = 0;
	for (int set = 0; set < 3; ++set)
		if (hashes[set] != expected[set])
			failed |= 1 << set;
	return failed;
#endif
}
