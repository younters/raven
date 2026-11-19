#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Several threads commit and decommit read-write pages inside ranges the runtime has already opened, which VirtualAlloc and VirtualFree
// may do in the guest by logging the change for the runtime, and each thread asks VirtualQuery after every change. The runtime takes the
// log before it answers a query, so with threads running at the same time a query by one thread can take the log while another thread is
// writing to it. Every answer must still describe the change that thread made last: a page committed and reported reserved, or
// decommitted and reported committed, is a lost or replayed log entry. Prints the rounds done and returns 0, or the failure.
// With --bench the threads only commit and decommit, a fixed number of rounds each, and the time taken is printed.

#define THREADS 8
#define ROUNDS 4000
#define RANGE (1024 * 1024)
#define PAGES (RANGE / 4096)
#define LIMIT_MS 20000

static volatile LONG stop, rounds, failure_thread, failure_round;
static volatile LONG failure;
static ULONGLONG deadline;
static int bench;

static void report(int code, unsigned thread, unsigned round)
{
	if (InterlockedCompareExchange(&failure, code, 0) == 0) {
		failure_thread = (LONG)thread;
		failure_round = (LONG)round;
	}
	stop = 1;
}

static DWORD WINAPI worker(LPVOID parameter)
{
	const unsigned id = (unsigned)(ULONG_PTR)parameter;
	BYTE *range = (BYTE *)VirtualAlloc(0, RANGE, MEM_RESERVE, PAGE_READWRITE);
	// Committing part of a granule opens the reserved pages around it for later commits.
	if (!range || VirtualAlloc(range, 4096, MEM_COMMIT, PAGE_READWRITE) != range) {
		report(1, id, 0);
		return 1;
	}
	for (unsigned round = 0; round < ROUNDS && !stop; ++round) {
		BYTE *page = range + 4096 * (1 + round % (PAGES - 1));
		MEMORY_BASIC_INFORMATION info;
		if (VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE) != page) {
			report(2, id, round);
			return 2;
		}
		page[17] = (BYTE)(round + id);
		if (bench) {
			if (!VirtualFree(page, 4096, MEM_DECOMMIT)) {
				report(3, id, round);
				return 3;
			}
			InterlockedIncrement(&rounds);
			continue;
		}
		if (VirtualQuery(page, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
			report(10, id, round);
			return 10;
		}
		if (!VirtualFree(page, 4096, MEM_DECOMMIT)) {
			report(3, id, round);
			return 3;
		}
		if (VirtualQuery(page, &info, sizeof(info)) != sizeof(info) || info.State != MEM_RESERVE) {
			report(11, id, round);
			return 11;
		}
		InterlockedIncrement(&rounds);
		if ((round & 63) == 63 && GetTickCount64() > deadline)
			break;
	}
	return VirtualFree(range, 0, MEM_RELEASE) ? 0 : 4;
}

static void write_text(HANDLE out, const char *text)
{
	DWORD length = 0, written;
	while (text[length])
		++length;
	WriteFile(out, text, length, &written, 0);
}

static void write_number(HANDLE out, unsigned value)
{
	char digits[12];
	int at = 11;
	digits[at] = 0;
	do {
		digits[--at] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	write_text(out, digits + at);
}

int raven_entry(void)
{
	HANDLE threads[THREADS], out = GetStdHandle(STD_OUTPUT_HANDLE);
	for (const char *argument = GetCommandLineA(); *argument; ++argument)
		if (argument[0] == '-' && argument[1] == '-' && argument[2] == 'b')
			bench = 1;
	const ULONGLONG start = GetTickCount64();
	deadline = start + LIMIT_MS;
	for (unsigned index = 0; index < THREADS; ++index) {
		threads[index] = CreateThread(0, 0, worker, (LPVOID)(ULONG_PTR)index, 0, 0);
		if (!threads[index])
			return 5;
	}
	if (WaitForMultipleObjects(THREADS, threads, TRUE, LIMIT_MS + 10000) != WAIT_OBJECT_0)
		return 6;
	const ULONGLONG elapsed = GetTickCount64() - start;
	int result = failure;
	for (unsigned index = 0; index < THREADS; ++index) {
		DWORD code = 0;
		if (!result && (!GetExitCodeThread(threads[index], &code) || code))
			result = code ? (int)code : 7;
		CloseHandle(threads[index]);
	}
	write_text(out, "commit race: ");
	write_number(out, (unsigned)rounds);
	write_text(out, " rounds in ");
	write_number(out, (unsigned)elapsed);
	write_text(out, bench ? " ms without queries" : " ms");
	if (failure) {
		write_text(out, failure == 10 ? ", a commit was lost by thread " : failure == 11 ? ", a decommit was lost by thread " :
			", an operation failed in thread ");
		write_number(out, (unsigned)failure_thread);
		write_text(out, " at round ");
		write_number(out, (unsigned)failure_round);
	}
	write_text(out, "\n");
	return result;
}
