// GetThreadContext and SetThreadContext on another thread. The exit code names the first check that failed.
//
// On a suspended thread the checks follow Microsoft Learn's GetThreadContext and SetThreadContext pages: a read gives the groups the
// flags name, and a set changes them, which the thread continues with when it is resumed ("The value of the ContextFlags member of this
// structure specifies which portions of a thread's context to set"; checks 4-13 and 19-22).
//
// The other checks pin Raven's own choices, not Windows behaviour. Windows leaves the context of a running thread undefined ("You
// cannot get a valid context for a running thread"; setting it gives "unpredictable" results); Raven pauses such a thread for the call,
// so a read gives the registers it holds and a set moves it on (checks 2-3 and 14-16). A thread's read of its own context succeeds, as
// documented, but Windows calls that context not valid, so its contents are not checked; a thread cannot set its own context (check
// 17); an ended thread's context reads as it was left and cannot be set, and a call that races with a thread's end does one of these
// (checks 24-28).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __x86_64__
// Read by the worker's assembly, so they keep their names and are never optimized away.
__attribute__((used)) volatile LONG stage;
__attribute__((used)) volatile DWORD64 seen_r12;
__attribute__((used)) volatile BYTE pattern[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
__attribute__((used)) volatile BYTE seen_xmm6[16], seen_xmm7[16];

extern char spin[], spin_end[], second[], third[];

// The worker keeps R12 and XMM6 at known values and spins without calls, so nothing but a context can change them. At `second` it
// reports R12 and XMM6, at `third` XMM7; each then spins again.
__attribute__((naked)) static DWORD WINAPI worker(void *unused)
{
	__asm__ volatile(
		"movabsq $0x1122334455667788, %r12\n"
		"movdqu pattern(%rip), %xmm6\n"
		"movl $1, stage(%rip)\n"
		".globl spin\nspin:\n"
		"pause\n"
		"jmp spin\n"
		".globl spin_end\nspin_end:\n"
		".globl second\nsecond:\n"
		"movq %r12, seen_r12(%rip)\n"
		"movdqu %xmm6, seen_xmm6(%rip)\n"
		"movl $2, stage(%rip)\n"
		"1: pause\n"
		"jmp 1b\n"
		".globl third\nthird:\n"
		"movdqu %xmm7, seen_xmm7(%rip)\n"
		"movl $3, stage(%rip)\n"
		"2: pause\n"
		"jmp 2b\n");
}

static BOOL same(const volatile BYTE *left, const void *right);

__attribute__((used)) volatile BYTE sleeper_xmm6[16];
__attribute__((used)) volatile DWORD64 sleeper_r12;

// This one spends its time in Sleep, inside the runtime, where a parallel thread's saved state holds its general registers alone; a
// long sleep and a short stretch outside it make a suspension land inside it almost always. Each time it comes back it reports R12 and
// XMM6.
__attribute__((naked)) static DWORD WINAPI sleeper(void *unused)
{
	__asm__ volatile(
		"movabsq $0x0102030405060708, %r12\n"
		"movdqu pattern(%rip), %xmm6\n"
		"subq $40, %rsp\n"
		"1: movl $50, %ecx\n"
		"call *__imp_Sleep(%rip)\n"
		"movq %r12, sleeper_r12(%rip)\n"
		"movdqu %xmm6, sleeper_xmm6(%rip)\n"
		"jmp 1b\n");
}

static BOOL reported(const BYTE *wanted)
{
	for (unsigned delay = 0; !same(sleeper_xmm6, wanted) && delay < 5000; ++delay)
		Sleep(1);
	return same(sleeper_xmm6, wanted);
}

// Runs a little, so a caller can catch it running, then returns.
static DWORD WINAPI brief(void *unused)
{
	for (volatile unsigned count = 0; count < 200000; ++count) {}
	return 7;
}

static DWORD WINAPI landed(void *value)
{
	ExitThread((DWORD)(ULONG_PTR)value);
	return 0;
}

static BOOL reach(LONG wanted)
{
	for (unsigned delay = 0; stage != wanted && delay < 5000; ++delay)
		Sleep(1);
	return stage == wanted;
}

static BOOL same(const volatile BYTE *left, const void *right)
{
	const BYTE *bytes = right;
	for (unsigned index = 0; index < 16; ++index)
		if (left[index] != bytes[index])
			return FALSE;
	return TRUE;
}

static CONTEXT context;

int raven_entry(void)
{
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread || !reach(1))
		return 1;
	// Running (Raven's choice): the read pauses the thread where it is and lets it go on.
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
	if (!GetThreadContext(thread, &context))
		return 2;
	if (context.Rip < (DWORD64)spin || context.Rip >= (DWORD64)spin_end || context.R12 != 0x1122334455667788ull ||
		!same(pattern, &context.Xmm6))
		return 3;
	// Suspended, with the control group alone: R12 and XMM6, which the context leaves out, are what the thread finds at `second`.
	if (SuspendThread(thread) != 0)
		return 4;
	context.ContextFlags = CONTEXT_CONTROL;
	if (!GetThreadContext(thread, &context))
		return 5;
	context.Rip = (DWORD64)second;
	if (!SetThreadContext(thread, &context))
		return 6;
	if (ResumeThread(thread) != 1 || !reach(2))
		return 7;
	if (seen_r12 != 0x1122334455667788ull || !same(seen_xmm6, (const void *)pattern))
		return 8;
	// Suspended, with the floating-point group: XMM7 arrives, and a read before the thread goes on gives back what was set.
	if (SuspendThread(thread) != 0)
		return 9;
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
	if (!GetThreadContext(thread, &context))
		return 10;
	for (unsigned index = 0; index < 16; ++index)
		((BYTE *)&context.Xmm7)[index] = (BYTE)(0xe0 + index);
	context.Rip = (DWORD64)third;
	if (!SetThreadContext(thread, &context))
		return 11;
	M128A wanted = context.Xmm7;
	SecureZeroMemory(&context, sizeof(context));
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
	if (!GetThreadContext(thread, &context) || context.Rip != (DWORD64)third || !same((BYTE *)&context.Xmm7, &wanted))
		return 12;
	if (ResumeThread(thread) != 1 || !reach(3) || !same(seen_xmm7, &wanted))
		return 13;
	// Running (Raven's choice): a set moves the thread on without a suspension, here into a function that ends it with the code
	// it is given.
	context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(thread, &context))
		return 14;
	context.Rip = (DWORD64)landed;
	context.Rcx = 321;
	if (!SetThreadContext(thread, &context))
		return 15;
	DWORD code = 0;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code != 321)
		return 16;
	// The caller cannot set its own context (Raven's choice).
	context.ContextFlags = CONTEXT_CONTROL;
	if (!GetThreadContext(GetCurrentThread(), &context) || SetThreadContext(GetCurrentThread(), &context) ||
		GetLastError() != ERROR_ACCESS_DENIED)
		return 17;
	CloseHandle(thread);
	// Suspended inside the runtime: the floating-point group is read whole and set alone, and the integer group is kept.
	BYTE initial[16], changed[16];
	for (unsigned index = 0; index < 16; ++index) {
		initial[index] = pattern[index];
		changed[index] = (BYTE)(0x5a ^ index);
	}
	thread = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
	if (!thread || !reported(initial))
		return 18;
	if (SuspendThread(thread) != 0)
		return 19;
	SecureZeroMemory(&context, sizeof(context));
	context.ContextFlags = CONTEXT_FLOATING_POINT;
	if (!GetThreadContext(thread, &context) || !same((BYTE *)&context.Xmm6, initial))
		return 20;
	for (unsigned index = 0; index < 16; ++index)
		((BYTE *)&context.Xmm6)[index] = changed[index];
	if (!SetThreadContext(thread, &context) || ResumeThread(thread) != 1 || !reported(changed))
		return 21;
	if (sleeper_r12 != 0x0102030405060708ull)
		return 22;
	if (!TerminateThread(thread, 0))
		return 23;
	CloseHandle(thread);
	// Ending (Raven's choice): reads and sets while threads end. A set that names no group writes back what the call read, so it does not
	// disturb the thread; it either does that or finds the thread ended. Once ended, a read still succeeds and a set fails.
	for (unsigned round = 0; round < 20; ++round) {
		thread = CreateThread(NULL, 0, brief, NULL, 0, NULL);
		if (!thread)
			return 24;
		for (unsigned tries = 0; tries < 2000 && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT; ++tries) {
			context.ContextFlags = CONTEXT_CONTROL;
			if (!GetThreadContext(thread, &context))
				return 25;
			context.ContextFlags = CONTEXT_AMD64;
			SetLastError(0);
			if (!SetThreadContext(thread, &context) && GetLastError() != ERROR_ACCESS_DENIED)
				return 26;
		}
		DWORD ended = 0;
		if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &ended) || ended != 7)
			return 27;
		context.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(thread, &context) || SetThreadContext(thread, &context) || GetLastError() != ERROR_ACCESS_DENIED)
			return 28;
		CloseHandle(thread);
	}
	return 0;
}
#else
int raven_entry(void)
{
	// A 32-bit caller has no thread context yet (Raven's limitation): both calls fail.
	CONTEXT context;
	context.ContextFlags = CONTEXT_CONTROL;
	if (GetThreadContext(GetCurrentThread(), &context) || GetLastError() != ERROR_NOT_SUPPORTED)
		return 1;
	SetLastError(0);
	if (SetThreadContext(GetCurrentThread(), &context) || GetLastError() != ERROR_NOT_SUPPORTED)
		return 2;
	return 0;
}
#endif
