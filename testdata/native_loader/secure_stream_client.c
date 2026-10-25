#include <windows.h>
#include <stdarg.h>

static int (__cdecl *print_ansi)(unsigned long long, void *, const char *, void *, va_list);
static int (__cdecl *print_wide)(unsigned long long, void *, const WCHAR *, void *, va_list);
static void *stream;

static int ansi(const char *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = print_ansi(0, stream, format, NULL, arguments);
	va_end(arguments);
	return result;
}

static int wide(const WCHAR *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = print_wide(0, stream, format, NULL, arguments);
	va_end(arguments);
	return result;
}

UINT raven_entry(void)
{
	HMODULE crt = LoadLibraryA("ucrtbase.dll");
	void *(__cdecl *iob)(unsigned);
	int *(__cdecl *error)(void);
	double (__cdecl *make_nan)(const char *);
	int (__cdecl *convert)(long, char *, size_t, int);
	int (__cdecl *upper)(WCHAR *, size_t);
	WCHAR *(__cdecl *token)(WCHAR *, const WCHAR *, WCHAR **);
	WCHAR *(__cdecl *locale)(int, const WCHAR *);
	char number[40];
	WCHAR text[] = L"a,,b", *context = NULL;
	if (!crt) return 1;
	print_ansi = (void *)GetProcAddress(crt, "__stdio_common_vfprintf_s");
	print_wide = (void *)GetProcAddress(crt, "__stdio_common_vfwprintf_s");
	iob = (void *)GetProcAddress(crt, "__acrt_iob_func");
	error = (void *)GetProcAddress(crt, "_errno");
	if (!print_ansi || !print_wide || !iob || !error) return 2;
	stream = iob(1);
	if (ansi("secure %d\n", 42) != 10) return 3;
	if (wide(L"wide %d\n", 42) != 8) return 4;
	if (ansi(NULL) != -1 || *error() != 22) return 5;
	if (wide(NULL) != -1 || *error() != 22) return 6;
	stream = NULL;
	if (ansi("invalid") != -1 || wide(L"invalid") != -1) return 7;
	make_nan = (void *)GetProcAddress(crt, "nan");
	convert = (void *)GetProcAddress(crt, "_ltoa_s");
	upper = (void *)GetProcAddress(crt, "_wcsupr_s");
	token = (void *)GetProcAddress(crt, "wcstok_s");
	if (!make_nan || !convert || !upper || !token) return 8;
	double value = make_nan("");
	if (value == value) return 9;
	if (convert((long)0x80000000, number, sizeof(number), 10) || number[0] != '-' || number[10] != '8' || number[11]) return 10;
	if (convert(-1, number, sizeof(number), 16) || number[0] != 'f' || number[7] != 'f' || number[8]) return 11;
	if (convert(123, number, 2, 10) != 34 || number[0] || convert(1, number, sizeof(number), 1) != 22) return 12;
	if (upper(text, 5) || text[0] != 'A' || text[3] != 'B') return 13;
	if (token(text, L",", &context) != text || text[1]) return 14;
	if (token(NULL, L",", &context) != text + 3 || token(NULL, L",", &context)) return 15;
	if (token(NULL, NULL, &context) || *error() != 22) return 16;
	locale = (void *)GetProcAddress(crt, "_wsetlocale");
	if (!locale || !locale(0, L"C") || locale(0, NULL)[0] != 'C' || locale(0, L"unsupported")) return 17;
	return 0;
}
