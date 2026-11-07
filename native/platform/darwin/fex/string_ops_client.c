/* rep movsb and rep stosb in a 32-bit process: every alignment, lengths around the chunk sizes, both directions and overlapping copies,
   hashed with the registers they leave, so engines can be compared bit for bit. A copy that runs into a guard page must stop with the
   registers at the first byte it could not write (built with RAVEN_STRING_GUARD). Build with RAVEN_STRING_PRINT to write the hash to
   C:\stringhash.txt. */
#include <windows.h>
#ifdef RAVEN_STRING_GUARD
#include <string.h>
#endif

static unsigned long long hash = 1469598103934665603ULL;
static void mix(const void *data, unsigned size)
{
	const unsigned char *bytes = data;
	for (unsigned i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
}

static unsigned char area[1024];

static void reset(void)
{
	for (unsigned i = 0; i < sizeof area; ++i)
		area[i] = (unsigned char)(i * 7 + 3);
}

static void copy(unsigned to, unsigned from, unsigned count, int backward)
{
	unsigned char *d = area + to;
	const unsigned char *s = area + from;
	unsigned n = count;
	if (backward)
		__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory", "cc");
	else
		__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	const unsigned registers[3] = {(unsigned)(d - area), (unsigned)(s - area), n};
	mix(registers, sizeof registers);
}

static void fill(unsigned to, unsigned count, unsigned char value, int backward)
{
	unsigned char *d = area + to;
	unsigned n = count;
	if (backward)
		__asm__ volatile("std\n\trep stosb\n\tcld" : "+D"(d), "+c"(n) : "a"(value) : "memory", "cc");
	else
		__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
	const unsigned registers[2] = {(unsigned)(d - area), n};
	mix(registers, sizeof registers);
}

static volatile unsigned fault_edi, fault_ecx;

static LONG CALLBACK guard_handler(EXCEPTION_POINTERS *info)
{
	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	fault_edi = info->ContextRecord->Edi;
	fault_ecx = info->ContextRecord->Ecx;
	/* Skip the rep movsb and leave the counter as it was. */
	info->ContextRecord->Eip += 2;
	return EXCEPTION_CONTINUE_EXECUTION;
}

int raven_entry(void)
{
	static const unsigned lengths[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 200, 255, 256, 257, 300};
	for (unsigned l = 0; l < sizeof lengths / sizeof *lengths; ++l)
		for (unsigned to = 0; to < 16; ++to)
			for (unsigned from = 0; from < 16; from += 3)
				for (int backward = 0; backward < 2; ++backward) {
					const unsigned count = lengths[l];
					/* Separate buffers, then source and destination overlapping in each direction. */
					reset();
					copy(400 + to + (backward ? count : 0), 16 + from + (backward ? count : 0), count, backward);
					mix(area, sizeof area);
					reset();
					copy(100 + to + (backward ? count : 0), 96 + from + (backward ? count : 0), count, backward);
					mix(area, sizeof area);
					reset();
					copy(96 + to + (backward ? count : 0), 100 + from + (backward ? count : 0), count, backward);
					mix(area, sizeof area);
					reset();
					fill(300 + to + (backward ? count : 0), count, (unsigned char)(to * 31 + l), backward);
					mix(area, sizeof area);
				}

#ifdef RAVEN_STRING_GUARD
	/* Exceptions reach a 32-bit program's handlers through the C runtime, which an import loads. */
	if (strlen("guard") != 5)
		return 9;
	/* A copy that runs into memory that is only reserved faults at its first byte there, with the registers stopped at that byte and
	   everything before it written. The boundary is 64 KiB, which the host's 16 KiB pages also protect. */
	unsigned char *pages = VirtualAlloc(NULL, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
	if (!pages || !VirtualAlloc(pages, 0x10000, MEM_COMMIT, PAGE_READWRITE))
		return 10;
	AddVectoredExceptionHandler(1, guard_handler);
	for (unsigned start = 0x10000 - 77; start < 0x10000 - 60; ++start) {
		unsigned char *d = pages + start;
		const unsigned char *s = area;
		unsigned n = 200;
		fault_edi = fault_ecx = 0;
		__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
		if (fault_edi != (unsigned)(pages + 0x10000) || fault_ecx != 200 - (0x10000 - start))
			return 11;
		for (unsigned i = start; i < 0x10000; ++i)
			if (pages[i] != area[i - start])
				return 12;
	}
	return 0;
#endif
#ifdef RAVEN_STRING_PRINT
	HANDLE out = CreateFileA("C:\\stringhash.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	char line[18];
	for (int digit = 0; digit < 16; ++digit)
		line[digit] = "0123456789abcdef"[(hash >> (60 - digit * 4)) & 15];
	line[16] = '\r';
	line[17] = '\n';
	DWORD written;
	WriteFile(out, line, 18, &written, 0);
	CloseHandle(out);
	return 0;
#else
	return hash == 0x5e20e68b10bcd4fdULL ? 0 : 1;
#endif
}
