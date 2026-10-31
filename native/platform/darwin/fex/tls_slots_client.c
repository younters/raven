// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD slots[1087];
static DWORD WINAPI reader(LPVOID unused)
{
	(void)unused;
	for (unsigned i = 0; i < 1087; ++i) {
		if (TlsGetValue(slots[i]) != NULL) return 1;
		if (!TlsSetValue(slots[i], (void *)(ULONG_PTR)(i + 2000))) return 2;
	}
	Sleep(10);
	for (unsigned i = 0; i < 1087; ++i)
		if (TlsGetValue(slots[i]) != (void *)(ULONG_PTR)(i + 2000)) return 3;
	return 0;
}
int raven_entry(void)
{
	// Slot zero belongs to system libraries; all remaining inline and expansion
	// slots must be available to the guest, independent of emulator TLS usage.
	for (unsigned i = 0; i < 1087; ++i) {
		slots[i] = TlsAlloc();
		if (slots[i] != i + 1 || TlsGetValue(slots[i]) != NULL) return 10;
		if (!TlsSetValue(slots[i], (void *)(ULONG_PTR)(i + 100))) return 11;
	}
	if (TlsAlloc() != TLS_OUT_OF_INDEXES) return 12;
	HANDLE thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
	if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return 13;
	DWORD code = 99;
	if (!GetExitCodeThread(thread, &code) || code) return 14;
	CloseHandle(thread);
	for (unsigned i = 0; i < 1087; ++i) {
		if (TlsGetValue(slots[i]) != (void *)(ULONG_PTR)(i + 100)) return 15;
		if (!TlsFree(slots[i])) return 16;
	}
	return 0;
}
