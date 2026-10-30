// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <limits.h>

static volatile int calls, failed, order[8], used;
static volatile BYTE *target;
static LONG breakpoint_filter(EXCEPTION_POINTERS *exception)
{
	++calls;
	CONTEXT *context = exception->ContextRecord;
	EXCEPTION_RECORD *record = exception->ExceptionRecord;
	if (record->ExceptionCode != EXCEPTION_BREAKPOINT || record->ExceptionAddress != (void *)(uintptr_t)context->Rip ||
		*(const BYTE *)(uintptr_t)context->Rip != 0xcc || calls != 1) {
		failed = 1;
		return EXCEPTION_EXECUTE_HANDLER;
	}
	context->Rip += 1;
	return EXCEPTION_CONTINUE_EXECUTION;
}
static int breakpoint(void)
{
	__try { __asm__ volatile("int3\n\tnop"); }
	__except(breakpoint_filter(GetExceptionInformation())) { return 1; }
	return failed || calls != 1 ? 2 : 0;
}
static LONG read_filter(EXCEPTION_POINTERS *exception)
{
	EXCEPTION_RECORD *record = exception->ExceptionRecord;
	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters != 2 ||
		record->ExceptionInformation[0] != 0 || record->ExceptionInformation[1] != (ULONG_PTR)target) failed = 1;
	++calls;
	return EXCEPTION_EXECUTE_HANDLER;
}
static int read_av(void)
{
	target = (volatile BYTE *)(uintptr_t)1;
	__try { failed = *target; return 1; }
	__except(read_filter(GetExceptionInformation())) { }
	return failed || calls != 1 ? 2 : 0;
}
static LONG nested_filter(EXCEPTION_POINTERS *exception)
{
	if (exception->ExceptionRecord->ExceptionCode != 0xe0421001) return EXCEPTION_CONTINUE_SEARCH;
	order[used++] = 1;
	__try { RaiseException(0xe0421002, 0, 0, NULL); }
	__except(GetExceptionCode() == 0xe0421002 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		order[used++] = 2;
	}
	order[used++] = 3;
	return EXCEPTION_EXECUTE_HANDLER;
}
static int nested(void)
{
	__try { RaiseException(0xe0421001, 0, 0, NULL); return 1; }
	__except(nested_filter(GetExceptionInformation())) { order[used++] = 4; }
	return used == 4 && order[0] == 1 && order[1] == 2 && order[2] == 3 && order[3] == 4 ? 0 : 2;
}
static int unhandled(void)
{
	SetUnhandledExceptionFilter(NULL);
	RaiseException(0xe0424242, EXCEPTION_NONCONTINUABLE, 0, NULL);
	return 1;
}
static LONG subpage_filter(EXCEPTION_POINTERS *exception)
{
	if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) failed = 1;
	++calls;
	return EXCEPTION_EXECUTE_HANDLER;
}
static int subpage(void)
{
	BYTE *memory = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	DWORD old;
	MEMORY_BASIC_INFORMATION first, second;
	if (!memory || !VirtualProtect(memory + 4096, 4096, PAGE_READONLY, &old) || old != PAGE_READWRITE) return 1;
	if (!VirtualQuery(memory, &first, sizeof(first)) || !VirtualQuery(memory + 4096, &second, sizeof(second)) ||
		first.Protect != PAGE_READWRITE || second.Protect != PAGE_READONLY) return 2;
	__try { *(volatile BYTE *)(memory + 4096) = 7; }
	__except(subpage_filter(GetExceptionInformation())) { }
#if EXPECT_SUBPAGE_FAULT
	if (failed || calls != 1) return 3; // Rosetta: real 4K protections.
#else
	if (failed || calls != 0 || memory[4096] != 7) return 3; // Documented Tier 0 union-of-four-pages gap.
#endif
	memory[0] = 9;
	if (!VirtualProtect(memory, 16384, PAGE_READWRITE, &old)) return 4;
	return VirtualFree(memory, 0, MEM_RELEASE) ? 0 : 5;
}
static int guard_unsupported(void)
{
	BYTE *memory = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	DWORD old = 0x12345678;
	MEMORY_BASIC_INFORMATION info;
	if (!memory) return 1;
	SetLastError(0);
	if (VirtualProtect(memory, 16384, PAGE_READWRITE | PAGE_GUARD, &old)) return 2;
	if (GetLastError() != ERROR_INVALID_PARAMETER || old != 0x12345678) return 3;
	if (!VirtualQuery(memory, &info, sizeof(info)) || info.Protect != PAGE_READWRITE) return 4;
	memory[0] = 5;
	return VirtualFree(memory, 0, MEM_RELEASE) ? 0 : 6;
}
static int divide_overflow(void)
{
	LONGLONG dividend = LLONG_MIN, divisor = -1;
	// Force the hardware instruction without invoking C signed-overflow UB.
	__try { __asm__ volatile("cqto\n\tidivq %%rcx" : "+a"(dividend) : "c"(divisor) : "rdx", "cc"); return 1; }
	__except(GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { }
	return 0;
}
int raven_entry(void)
{
#if REVIEW_CASE == 1
	return breakpoint();
#elif REVIEW_CASE == 2
	return read_av();
#elif REVIEW_CASE == 3
	return nested();
#elif REVIEW_CASE == 4
	return unhandled();
#elif REVIEW_CASE == 5
	return subpage();
#elif REVIEW_CASE == 6
	return guard_unsupported();
#elif REVIEW_CASE == 7
	return divide_overflow();
#else
#error Define REVIEW_CASE
#endif
}
