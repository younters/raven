// Manual compute benchmark: run the same fixture with RAVEN_PARALLEL unset and with RAVEN_PARALLEL=1. Each worker has a dependent
// integer recurrence so the work cannot be vectorized, shared or optimized out; the host sees only thread creation and completion.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile ULONGLONG results[4];

static DWORD WINAPI compute(void *raw)
{
	const unsigned index = (unsigned)(ULONG_PTR)raw;
	ULONGLONG value = 123 + index;
	for (unsigned count = 0; count < 400000000; ++count)
		value = value * 6364136223846793005ULL + 1;
	results[index] = value;
	return 0;
}

int raven_entry(void)
{
	HANDLE workers[4];
	for (unsigned index = 0; index < 4; ++index) {
		workers[index] = CreateThread(0, 0, compute, (void *)(ULONG_PTR)index, 0, 0);
		if (!workers[index]) return 1;
	}
	if (WaitForMultipleObjects(4, workers, TRUE, 30000) != WAIT_OBJECT_0) return 2;
	for (unsigned index = 0; index < 4; ++index) {
		DWORD code = 99;
		if (!GetExitCodeThread(workers[index], &code) || code || !results[index]) return 3;
		CloseHandle(workers[index]);
	}
	return 0;
}
