// Manual transport benchmark: two Windows threads exchange auto-reset events. QPC timestamps are taken in the guest, around the
// whole request/reply rather than around individual host handlers. Output is count, median microseconds and p95 microseconds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define SAMPLES 2000
#define WARMUP 100
static HANDLE events[2];
static LONGLONG samples[SAMPLES];

static DWORD WINAPI worker(void *unused)
{
	(void)unused;
	for (unsigned i = 0; i < SAMPLES + WARMUP; ++i) {
		if (WaitForSingleObject(events[0], 10000) != WAIT_OBJECT_0) return 1;
		if (!SetEvent(events[1])) return 2;
	}
	return 0;
}

static void number(char **output, ULONGLONG value)
{
	char digits[24];
	unsigned count = 0;
	do {
		digits[count++] = '0' + value % 10;
		value /= 10;
	} while (value);
	while (count) *(*output)++ = digits[--count];
}

int raven_entry(void)
{
	events[0] = CreateEventW(0, FALSE, FALSE, 0);
	events[1] = CreateEventW(0, FALSE, FALSE, 0);
	HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
	if (!events[0] || !events[1] || !thread) return 1;
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency)) return 2;
	for (unsigned i = 0; i < SAMPLES + WARMUP; ++i) {
		LARGE_INTEGER before, after;
		QueryPerformanceCounter(&before);
		if (!SetEvent(events[0]) || WaitForSingleObject(events[1], 10000) != WAIT_OBJECT_0) return 3;
		QueryPerformanceCounter(&after);
		if (i >= WARMUP) samples[i - WARMUP] = after.QuadPart - before.QuadPart;
	}
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return 4;
	DWORD code;
	if (!GetExitCodeThread(thread, &code) || code) return 5;
	// Sorting is outside the measured region and needs no guest C runtime.
	for (unsigned i = 1; i < SAMPLES; ++i) {
		const LONGLONG value = samples[i];
		unsigned j = i;
		while (j && samples[j - 1] > value) { samples[j] = samples[j - 1]; --j; }
		samples[j] = value;
	}
	char output[128], *next = output;
	number(&next, SAMPLES); *next++ = ' ';
	number(&next, (ULONGLONG)samples[SAMPLES / 2] * 1000000 / frequency.QuadPart); *next++ = ' ';
	number(&next, (ULONGLONG)samples[SAMPLES * 95 / 100] * 1000000 / frequency.QuadPart); *next++ = '\n';
	DWORD written;
	if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, (DWORD)(next - output), &written, 0)) return 6;
	CloseHandle(thread);
	CloseHandle(events[0]);
	CloseHandle(events[1]);
	return 0;
}
