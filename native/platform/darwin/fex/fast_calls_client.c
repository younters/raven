/* The calls a runtime answers in the guest behave as Windows's do, and stay in the guest: a served call costs a thousand times as much. */
#include <windows.h>

#ifdef _WIN64
#define TLS_SLOT_OF(index) (*(void **)(__readgsqword(0x30) + 0x1480 + (index) * 8))
#else
#define TLS_SLOT_OF(index) (*(void **)(__readfsdword(0x18) + 0xe10 + (index) * 4))
#endif

static DWORD shared_slot, freed_slot, fiber_slot;
static CRITICAL_SECTION section;
static volatile LONG counter_value;
static volatile LONG try_result = -1;
static HANDLE go, done;

static void WINAPI fiber_callback(void *value)
{
	(void)value;
}

static DWORD WINAPI worker(void *argument)
{
	(void)argument;
	/* Each thread has its own values. */
	if (TlsGetValue(shared_slot) != 0)
		return 20;
	TlsSetValue(shared_slot, (void *)0x22);
	if (TlsGetValue(shared_slot) != (void *)0x22 || TLS_SLOT_OF(shared_slot) != (void *)0x22)
		return 21;
	if (GetLastError() != 0)
		return 22;
	SetLastError(0x1234);
	if (GetLastError() != 0x1234)
		return 23;
	if (FlsGetValue(fiber_slot) != 0)
		return 24;
	FlsSetValue(fiber_slot, (void *)0x77);
	if (FlsGetValue(fiber_slot) != (void *)0x77)
		return 25;
	/* A slot freed on one thread and taken again reads zero on every thread. */
	TlsSetValue(freed_slot, (void *)0x33);
	SetEvent(done);
	WaitForSingleObject(go, 10000);
	if (TlsGetValue(freed_slot) != 0)
		return 26;
	return 0;
}

/* Four threads add to a plain counter under one section: any lost update shows a section that let two in. */
static DWORD WINAPI adder(void *argument)
{
	(void)argument;
	for (int i = 0; i < 20000; ++i) {
		EnterCriticalSection(&section);
		LONG value = counter_value;
		for (volatile int spin = 0; spin < 3; ++spin) {}
		counter_value = value + 1;
		LeaveCriticalSection(&section);
	}
	return 0;
}

static DWORD WINAPI try_other(void *argument)
{
	(void)argument;
	try_result = TryEnterCriticalSection(&section);
	if (try_result)
		LeaveCriticalSection(&section);
	return 0;
}

typedef LONG(WINAPI *one_word)(volatile LONG *);
typedef LONG(WINAPI *two_words)(volatile LONG *, LONG);
typedef LONG(WINAPI *three_words)(volatile LONG *, LONG, LONG);

int raven_entry(void)
{
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	/* Answered in the guest: an entry that begins with the breakpoint would go to the runtime. */
	static const char *const fast[] = {"GetLastError", "SetLastError", "GetCurrentThreadId", "GetCurrentProcessId", "GetCurrentProcess",
		"GetCurrentThread", "TlsGetValue", "TlsSetValue", "FlsGetValue", "EnterCriticalSection", "LeaveCriticalSection",
		"TryEnterCriticalSection"};
	for (unsigned i = 0; i < sizeof fast / sizeof *fast; ++i) {
		const unsigned char *code = (const unsigned char *)GetProcAddress(kernel, fast[i]);
		if (!code)
			ExitProcess(100 + i);
		if (*code == 0xcc)
			ExitProcess(120 + i);
	}

	SetLastError(5);
	if (GetLastError() != 5)
		ExitProcess(1);
	if (GetCurrentProcess() != (HANDLE)(LONG_PTR)-1 || GetCurrentThread() != (HANDLE)(LONG_PTR)-2)
		ExitProcess(2);
	if (!GetCurrentThreadId() || !GetCurrentProcessId())
		ExitProcess(3);

	shared_slot = TlsAlloc();
	freed_slot = TlsAlloc();
	fiber_slot = FlsAlloc(fiber_callback);
	if (shared_slot == TLS_OUT_OF_INDEXES || freed_slot == TLS_OUT_OF_INDEXES || fiber_slot == FLS_OUT_OF_INDEXES)
		ExitProcess(4);
	if (!shared_slot)
		ExitProcess(5);
	TlsSetValue(shared_slot, (void *)0x11);
	SetLastError(9);
	if (TlsGetValue(shared_slot) != (void *)0x11 || TLS_SLOT_OF(shared_slot) != (void *)0x11)
		ExitProcess(6);
	/* A successful read clears the last error, which programs depend on to tell a stored zero from a failure. */
	if (GetLastError() != 0)
		ExitProcess(7);
	/* The slots past the 64 in the thread block work the same. */
	DWORD slots[80];
	for (unsigned i = 0; i < 80; ++i) {
		slots[i] = TlsAlloc();
		if (slots[i] == TLS_OUT_OF_INDEXES)
			ExitProcess(8);
		if (!TlsSetValue(slots[i], (void *)(ULONG_PTR)(i + 1)))
			ExitProcess(9);
	}
	for (unsigned i = 0; i < 80; ++i)
		if (TlsGetValue(slots[i]) != (void *)(ULONG_PTR)(i + 1))
			ExitProcess(10);
	for (unsigned i = 0; i < 80; ++i)
		TlsFree(slots[i]);
	if (!FlsSetValue(fiber_slot, (void *)0x44) || FlsGetValue(fiber_slot) != (void *)0x44)
		ExitProcess(11);

	go = CreateEventA(NULL, TRUE, FALSE, NULL);
	done = CreateEventA(NULL, TRUE, FALSE, NULL);
	DWORD id = 0;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, &id);
	if (!thread || id == GetCurrentThreadId())
		ExitProcess(12);
	if (WaitForSingleObject(done, 10000) != WAIT_OBJECT_0)
		ExitProcess(13);
	/* The worker's values did not reach this thread. */
	if (TlsGetValue(shared_slot) != (void *)0x11 || FlsGetValue(fiber_slot) != (void *)0x44)
		ExitProcess(14);
	TlsFree(freed_slot);
	if (TlsAlloc() != freed_slot)
		ExitProcess(15);
	SetEvent(go);
	DWORD status = 1;
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &status))
		ExitProcess(16);
	if (status)
		ExitProcess(status);

	/* A critical section: its owner and depth where Windows keeps them, recursion, a try from another thread, and exclusion. */
	InitializeCriticalSection(&section);
	EnterCriticalSection(&section);
	EnterCriticalSection(&section);
	if (!TryEnterCriticalSection(&section))
		ExitProcess(40);
	if ((DWORD)(ULONG_PTR)section.OwningThread != GetCurrentThreadId() || section.RecursionCount != 3)
		ExitProcess(41);
	HANDLE other = CreateThread(NULL, 0, try_other, NULL, 0, NULL);
	if (!other || WaitForSingleObject(other, 10000) != WAIT_OBJECT_0 || try_result != 0)
		ExitProcess(42);
	CloseHandle(other);
	LeaveCriticalSection(&section);
	LeaveCriticalSection(&section);
	LeaveCriticalSection(&section);
	if (section.OwningThread || section.RecursionCount)
		ExitProcess(43);
	HANDLE adders[4];
	for (int i = 0; i < 4; ++i)
		if (!(adders[i] = CreateThread(NULL, 0, adder, NULL, 0, NULL)))
			ExitProcess(44);
	if (WaitForMultipleObjects(4, adders, TRUE, 60000) != WAIT_OBJECT_0)
		ExitProcess(45);
	if (counter_value != 80000)
		ExitProcess(46);
	DeleteCriticalSection(&section);

#ifndef _WIN64
	/* 32-bit kernel32 exports the interlocked calls, whose results programs rely on. */
	volatile LONG number = 5;
	if (((one_word)GetProcAddress(kernel, "InterlockedIncrement"))(&number) != 6 || number != 6)
		ExitProcess(30);
	if (((one_word)GetProcAddress(kernel, "InterlockedDecrement"))(&number) != 5 || number != 5)
		ExitProcess(31);
	if (((two_words)GetProcAddress(kernel, "InterlockedExchange"))(&number, 9) != 5 || number != 9)
		ExitProcess(32);
	if (((two_words)GetProcAddress(kernel, "InterlockedExchangeAdd"))(&number, 3) != 9 || number != 12)
		ExitProcess(33);
	if (((three_words)GetProcAddress(kernel, "InterlockedCompareExchange"))(&number, 1, 12) != 12 || number != 1)
		ExitProcess(34);
	if (((three_words)GetProcAddress(kernel, "InterlockedCompareExchange"))(&number, 2, 12) != 1 || number != 1)
		ExitProcess(35);
#endif
	return 0;
}
