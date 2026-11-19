// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD WINAPI worker(LPVOID argument)
{
	BYTE *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!code) return 1;
	for (unsigned round = 0; round < 64; ++round) {
		unsigned value = 100 + round + (unsigned)(ULONG_PTR)argument;
		code[0] = 0xb8;
		*(unsigned *)(code + 1) = value;
		code[5] = 0xc3;
		if (!FlushInstructionCache(GetCurrentProcess(), code, 6)) return 2;
		if (((unsigned (*)(void))code)() != value) return 3;
		void *memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 700 + round);
		if (!memory || !HeapFree(GetProcessHeap(), 0, memory)) return 4;
		Sleep(0);
	}
	return VirtualFree(code, 0, MEM_RELEASE) ? 0 : 5;
}
int raven_entry(void)
{
	HANDLE threads[8];
	for (unsigned i = 0; i < 8; ++i)
		if (!(threads[i] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)i, 0, NULL))) return 10;
	if (WaitForMultipleObjects(8, threads, TRUE, 30000) != WAIT_OBJECT_0) return 11;
	for (unsigned i = 0; i < 8; ++i) {
		DWORD code = 99;
		if (!GetExitCodeThread(threads[i], &code) || code) return 12;
		CloseHandle(threads[i]);
	}
	return 0;
}
