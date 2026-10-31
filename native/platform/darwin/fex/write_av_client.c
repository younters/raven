// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void *target;
static int filter(EXCEPTION_POINTERS *exception)
{
	EXCEPTION_RECORD *record = exception->ExceptionRecord;
	return record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters == 2 &&
		record->ExceptionInformation[0] == 1 && record->ExceptionInformation[1] == (ULONG_PTR)target ?
		EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
int raven_entry(void)
{
	DWORD old;
	target = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!target) return 1;
	// Protect a whole host page so Tier 0's documented 4K gap is not involved.
	if (!VirtualProtect(target, 16384, PAGE_READONLY, &old)) return 2;
	int caught = 0;
	__try {
		*(volatile BYTE *)target = 7;
	} __except(filter(GetExceptionInformation())) {
		caught = 1;
	}
	if (!caught) return 3;
	// Reserved guest memory is not a FEX internal overcommit allocation.
	void *reserved = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_READWRITE);
	if (!reserved) return 5;
	target = reserved;
	caught = 0;
	__try { *(volatile BYTE *)target = 7; }
	__except(filter(GetExceptionInformation())) { caught = 1; }
	if (!caught || !VirtualFree(reserved, 0, MEM_RELEASE)) return 6;
	// Unaligned TSO repair must preserve a catchable guest AV, even if its
	// software decoder faults while accessing the protected operand.
	target = (BYTE *)VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	void *allocation = target;
	if (!target || !VirtualProtect(target, 16384, PAGE_NOACCESS, &old)) return 7;
	target = (BYTE *)target + 1;
	caught = 0;
	__try { *(volatile ULONG_PTR *)target = 7; }
	__except(filter(GetExceptionInformation())) { caught = 1; }
	if (!caught) return 8;
	target = allocation;
	return VirtualFree(target, 0, MEM_RELEASE) ? 0 : 4;
}
