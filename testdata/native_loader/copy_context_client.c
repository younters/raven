#include <windows.h>

/* CopyContext from a program of either width: it copies the parts it is asked for into a destination of the program's own CONTEXT size,
   names them in the destination's flags, writes nothing past the destination and refuses a missing context. A 32-bit CONTEXT is 0x2cc
   bytes and a 64-bit one 0x4d0; the guard after the destination catches a copy of the wrong size. */

typedef BOOL(WINAPI *copy_context_fn)(PCONTEXT, DWORD, PCONTEXT);

_Static_assert(sizeof(CONTEXT) == (sizeof(void *) == 8 ? 0x4d0 : 0x2cc), "CONTEXT size");

static CONTEXT source;
static struct {
	CONTEXT context;
	unsigned char guard[256];
} destination;

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

unsigned int raven_entry(void)
{
	const copy_context_fn copy = (copy_context_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CopyContext");
	CHECK(copy);

	SetLastError(0);
	CHECK(!copy(NULL, CONTEXT_INTEGER, &source) && GetLastError() == ERROR_INVALID_PARAMETER);
	SetLastError(0);
	CHECK(!copy(&destination.context, CONTEXT_INTEGER, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);

	source.ContextFlags = CONTEXT_FULL;
#ifdef _WIN64
	source.Rax = 0x1122334455667788ULL;
	source.R15 = 0x0123456789abcdefULL;
	source.Rip = 0x140001000ULL;
#else
	source.Eax = 0x11223344;
	source.Edi = 0x55667788;
	source.Eip = 0x401000;
#endif
	destination.context.ContextFlags = CONTEXT_CONTROL;
	for (unsigned int i = 0; i < sizeof destination.guard; ++i)
		destination.guard[i] = 0xcc;

	CHECK(copy(&destination.context, CONTEXT_INTEGER, &source));
	CHECK((destination.context.ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER);
#ifdef _WIN64
	CHECK(destination.context.Rax == source.Rax && destination.context.R15 == source.R15);
#else
	CHECK(destination.context.Eax == source.Eax && destination.context.Edi == source.Edi);
#endif
	for (unsigned int i = 0; i < sizeof destination.guard; ++i)
		CHECK(destination.guard[i] == 0xcc);
	return 0;
}
