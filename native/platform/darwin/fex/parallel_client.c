// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
static volatile LONG arrived, counter, release_worker, seen_apc;
static volatile DWORD worker_id;
static HANDLE event;
static DWORD WINAPI contention(void *arg)
{
	(void)arg;
	__atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
	// No API calls: with time slicing disabled, both guests must execute on
	// different host threads to cross this barrier.
	while (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) != 2) __asm__ volatile("pause");
	for (int i = 0; i < 100000; ++i) __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
	return 37;
}
static DWORD WINAPI spinner(void *arg)
{
	(void)arg;
	worker_id = GetCurrentThreadId();
	SetEvent(event);
	while (!__atomic_load_n(&release_worker, __ATOMIC_SEQ_CST)) __asm__ volatile("pause");
#if M3_CASE == 2
	uint64_t changed;
	__asm__ volatile("movq %%r12,%0" : "=r"(changed));
	if (changed != 0x123456789abcdef0ULL) return 2;
#endif
	return 73;
}
static void CALLBACK apc(ULONG_PTR arg)
{
	if (GetCurrentThreadId() == worker_id) seen_apc = (LONG)arg;
}
static DWORD WINAPI alertable(void *arg)
{
	(void)arg;
	worker_id = GetCurrentThreadId();
	SetEvent(event);
	return SleepEx(5000, TRUE) == WAIT_IO_COMPLETION && seen_apc == 91 ? 19 : 1;
}
static DWORD WINAPI exceptions(void *arg)
{
	(void)arg;
	__try { *(volatile int *)(uintptr_t)1 = 42; return 1; }
	__except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
	__try { RaiseException(0xe0423001, 0, 0, NULL); return 2; }
	__except(GetExceptionCode() == 0xe0423001 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
	return 81;
}
static DWORD WINAPI exited(void *arg)
{
	(void)arg;
	ExitThread(55);
	return 1;
}
static DWORD WINAPI unhandled(void *arg)
{
	(void)arg;
	RaiseException(0xe0424242, 0, 0, NULL);
	return 1;
}
static int joined(HANDLE thread, DWORD expected)
{
	DWORD code = 0;
	int ok = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && code == expected;
	CloseHandle(thread);
	return ok;
}
int raven_entry(void)
{
#if M3_CASE == 1
	HANDLE a = CreateThread(NULL, 0, contention, NULL, 0, NULL);
	HANDLE b = CreateThread(NULL, 0, contention, NULL, 0, NULL);
	CHECK(a && b && joined(a, 37) && joined(b, 37));
	CHECK(counter == 200000);
#elif M3_CASE == 2
	event = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE a = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
	CHECK(a && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
	CHECK(SuspendThread(a) == 0);
	static CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	CHECK(GetThreadContext(a, &ctx) && ctx.Rip && ctx.Rsp);
	ctx.R12 = 0x123456789abcdef0ULL;
	CHECK(SetThreadContext(a, &ctx));
	static CONTEXT readback;
	readback.ContextFlags = ctx.ContextFlags;
	CHECK(GetThreadContext(a, &readback) && readback.R12 == ctx.R12);
	CHECK(SuspendThread(a) == 1 && ResumeThread(a) == 2);
	__atomic_store_n(&release_worker, 1, __ATOMIC_SEQ_CST);
	CHECK(WaitForSingleObject(a, 0) == WAIT_TIMEOUT);
	CHECK(ResumeThread(a) == 1 && joined(a, 73));
	CloseHandle(event);
#elif M3_CASE == 3
	CHECK(joined(CreateThread(NULL, 0, exceptions, NULL, 0, NULL), 81));
#elif M3_CASE == 4
	event = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE a = CreateThread(NULL, 0, alertable, NULL, 0, NULL);
	CHECK(a && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
	CHECK(QueueUserAPC(apc, a, 91) && joined(a, 19));
	CloseHandle(event);
#elif M3_CASE == 5
	event = CreateEventW(NULL, FALSE, FALSE, NULL);
	for (int i = 0; i < 64; ++i) {
		release_worker = 1;
		HANDLE a = CreateThread(NULL, 0, spinner, NULL, CREATE_SUSPENDED, NULL);
		CHECK(a && ResumeThread(a) == 1 && joined(a, 73));
		CHECK(joined(CreateThread(NULL, 0, exited, NULL, 0, NULL), 55));
	}
	CHECK(ResetEvent(event));
	release_worker = 0;
	HANDLE a = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
	CHECK(a && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
	CHECK(TerminateThread(a, 29) && joined(a, 29));
	CloseHandle(event);
#elif M3_CASE == 6
	CHECK(joined(CreateThread(NULL, 0, unhandled, NULL, 0, NULL), 0));
	return 99; // An unhandled worker exception must end the process.
#endif
	return 0;
}
