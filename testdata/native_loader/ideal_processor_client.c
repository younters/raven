#include <windows.h>

/* A thread's ideal processor. Documented on Microsoft Learn: SetThreadIdealProcessor returns the previous processor, MAXIMUM_PROCESSORS
 * only asks, it fails with (DWORD)-1; the Ex calls take a PROCESSOR_NUMBER, the previous one may be the request itself, and they fail
 * with FALSE; setting needs THREAD_SET_INFORMATION and asking THREAD_QUERY_LIMITED_INFORMATION. Raven's choice: a processor
 * GetSystemInfo did not report or another group is ERROR_INVALID_PARAMETER, a closed handle or one that is not a thread's
 * ERROR_INVALID_HANDLE, a handle without the access ERROR_ACCESS_DENIED, and the value is kept per thread for this process's threads and
 * per handle object for another process's. Started with "c" the program ends at once; with "r <handle>" it checks an inherited handle to
 * a thread of its parent. Returns 0, or the number of the check that failed (30 and 31 come right after 9). */

static HANDLE go;

static DWORD WINAPI waiter(void *parameter)
{
	(void)parameter;
	WaitForSingleObject(go, INFINITE);
	return 0;
}

static const WCHAR *arguments(void)
{
	const WCHAR *line = GetCommandLineW();
	if (*line == L'"') {
		for (++line; *line && *line != L'"'; ++line)
			;
		if (*line)
			++line;
	}
	while (*line == L' ')
		++line;
	return line;
}

static WCHAR *hex(WCHAR *at, ULONG_PTR value)
{
	WCHAR digits[17];
	int count = 0;
	do
		digits[count++] = L"0123456789abcdef"[value & 15];
	while (value >>= 4);
	while (count)
		*at++ = digits[--count];
	return at;
}

/* The same hint read and written through one handle, whatever kind of thread it names. */
static int round_trip(HANDLE thread, DWORD wanted)
{
	if (SetThreadIdealProcessor(thread, wanted) == (DWORD)-1 || SetThreadIdealProcessor(thread, MAXIMUM_PROCESSORS) != wanted)
		return 1;
	PROCESSOR_NUMBER number = {7, 7, 7};
	if (!GetThreadIdealProcessorEx(thread, &number) || number.Group || number.Number != wanted)
		return 2;
	PROCESSOR_NUMBER request = {0, 0, 0}, previous = {7, 7, 7};
	if (!SetThreadIdealProcessorEx(thread, &request, &previous) || previous.Group || previous.Number != wanted ||
		SetThreadIdealProcessor(thread, MAXIMUM_PROCESSORS) != 0 || SetThreadIdealProcessor(thread, wanted) != 0)
		return 3;
	return 0;
}

/* The child started with "r": the handle it inherited names a thread of its parent, another process. */
static UINT remote(const WCHAR *at)
{
	ULONG_PTR value = 0;
	for (; (*at >= L'0' && *at <= L'9') || (*at >= L'a' && *at <= L'f'); ++at)
		value = value * 16 + (*at <= L'9' ? *at - L'0' : *at - L'a' + 10);
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	return round_trip((HANDLE)value, system.dwNumberOfProcessors - 1) ? 40 : 0;
}

static BOOL start_child(const WCHAR *tail, BOOL inherit, DWORD flags, PROCESS_INFORMATION *child)
{
	static WCHAR path[MAX_PATH], line[MAX_PATH + 40];
	const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return FALSE;
	WCHAR *at = line;
	*at++ = L'"';
	for (DWORD index = 0; index < length; ++index)
		*at++ = path[index];
	*at++ = L'"';
	*at++ = L' ';
	while (*tail)
		*at++ = *tail++;
	*at = 0;
	static STARTUPINFOW startup;
	startup.cb = sizeof startup;
	return CreateProcessW(path, line, NULL, NULL, inherit, flags, NULL, NULL, &startup, child);
}

UINT raven_entry(void)
{
	const WCHAR *given = arguments();
	if (given[0] == L'c' && !given[1])
		ExitProcess(0);
	if (given[0] == L'r' && given[1] == L' ')
		ExitProcess(remote(given + 2));

	HANDLE self = GetCurrentThread();
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	/* The test works from whatever processor the thread has and moves it to another one. With a single processor there is no other,
	 * and the checks only see the value kept. */
	const DWORD start = SetThreadIdealProcessor(self, MAXIMUM_PROCESSORS);
	const DWORD other = start ? 0 : system.dwNumberOfProcessors - 1;
	if (start >= system.dwNumberOfProcessors)
		return 1;
	if (SetThreadIdealProcessor(self, other) != start || SetThreadIdealProcessor(self, MAXIMUM_PROCESSORS) != other)
		return 2;
	SetLastError(0);
	if (system.dwNumberOfProcessors < MAXIMUM_PROCESSORS &&
		(SetThreadIdealProcessor(self, system.dwNumberOfProcessors) != (DWORD)-1 || GetLastError() != ERROR_INVALID_PARAMETER))
		return 3;
	PROCESSOR_NUMBER number = {7, 7, 7};
	if (!GetThreadIdealProcessorEx(self, &number) || number.Group || number.Number != other)
		return 4;
	PROCESSOR_NUMBER first = {0, (BYTE)start, 0}, previous = {7, 7, 7};
	if (!SetThreadIdealProcessorEx(self, &first, &previous) || previous.Group || previous.Number != other)
		return 5;
	if (SetThreadIdealProcessor(self, MAXIMUM_PROCESSORS) != start)
		return 6;
	/* The request and the previous processor may be the same structure. */
	PROCESSOR_NUMBER both = {0, (BYTE)other, 0};
	if (!SetThreadIdealProcessorEx(self, &both, &both) || both.Group || both.Number != start ||
		SetThreadIdealProcessor(self, MAXIMUM_PROCESSORS) != other || SetThreadIdealProcessor(self, start) != other)
		return 7;
	PROCESSOR_NUMBER other_group = {1, 0, 0}, missing = {0, (BYTE)system.dwNumberOfProcessors, 0};
	SetLastError(0);
	if (SetThreadIdealProcessorEx(self, &other_group, NULL) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 8;
	SetLastError(0);
	if (SetThreadIdealProcessorEx(self, &missing, NULL) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 9;
	/* Raven's choice: the processors accepted are exactly those GetSystemInfo reports, every one below dwNumberOfProcessors, and
	 * dwActiveProcessorMask and the masks GetProcessAffinityMask gives name the same processors. */
	DWORD_PTR process_mask = 0, system_mask = 0;
	const DWORD_PTR all = system.dwNumberOfProcessors >= sizeof(DWORD_PTR) * 8 ? ~(DWORD_PTR)0
																				: ((DWORD_PTR)1 << system.dwNumberOfProcessors) - 1;
	if (!system.dwNumberOfProcessors || system.dwNumberOfProcessors > MAXIMUM_PROCESSORS || system.dwActiveProcessorMask != all ||
		!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || process_mask != all || system_mask != all)
		return 30;
	for (DWORD processor = 0; processor < system.dwNumberOfProcessors; ++processor) {
		PROCESSOR_NUMBER each = {0, (BYTE)processor, 0};
		if (SetThreadIdealProcessor(self, processor) == (DWORD)-1 || !SetThreadIdealProcessorEx(self, &each, NULL) ||
			!GetThreadIdealProcessorEx(self, &number) || number.Number != processor)
			return 31;
	}
	if (SetThreadIdealProcessor(self, start) == (DWORD)-1)
		return 31;

	/* A second thread of this process keeps its own value: setting it leaves this thread's alone, and setting this one leaves it. */
	go = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE second = go ? CreateThread(NULL, 0, waiter, NULL, 0, NULL) : NULL;
	if (!second)
		return 10;
	if (SetThreadIdealProcessor(second, other) == (DWORD)-1 || SetThreadIdealProcessor(self, MAXIMUM_PROCESSORS) != start)
		return 11;
	if (SetThreadIdealProcessor(self, start) != start || !GetThreadIdealProcessorEx(second, &number) || number.Number != other)
		return 12;
	HANDLE opened = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetThreadId(second));
	if (!opened || SetThreadIdealProcessor(opened, MAXIMUM_PROCESSORS) != other)
		return 13;
	CloseHandle(opened);

	/* The main thread of a child, through the handle CreateProcess gave: what is set reads back. */
	PROCESS_INFORMATION child;
	DWORD code = 1;
	if (!start_child(L"c", FALSE, CREATE_SUSPENDED, &child))
		return 14;
	if (round_trip(child.hThread, system.dwNumberOfProcessors - 1))
		return 15;
	if (ResumeThread(child.hThread) == (DWORD)-1 || WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(child.hProcess, &code) || code)
		return 16;
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);

	/* A child that inherits a handle to the second thread: what it sets reads back through that handle. */
	HANDLE inheritable;
	if (!DuplicateHandle(GetCurrentProcess(), second, GetCurrentProcess(), &inheritable, 0, TRUE, DUPLICATE_SAME_ACCESS))
		return 17;
	static WCHAR tail[24];
	tail[0] = L'r';
	tail[1] = L' ';
	*hex(tail + 2, (ULONG_PTR)inheritable) = 0;
	if (!start_child(tail, TRUE, 0, &child))
		return 18;
	if (WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code) || code)
		return code ? code : 19;
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	CloseHandle(inheritable);
	SetEvent(go);
	WaitForSingleObject(second, 30000);
	CloseHandle(second);
	CloseHandle(go);

	/* A thread handle that has been closed. */
	HANDLE copy;
	if (!DuplicateHandle(GetCurrentProcess(), self, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return 20;
	CloseHandle(copy);
	SetLastError(0);
	if (SetThreadIdealProcessor(copy, start) != (DWORD)-1 || GetLastError() != ERROR_INVALID_HANDLE)
		return 21;
	SetLastError(0);
	if (GetThreadIdealProcessorEx(copy, &number) || GetLastError() != ERROR_INVALID_HANDLE)
		return 22;
	/* A handle that is not a thread's, and thread handles without THREAD_SET_INFORMATION or THREAD_QUERY_LIMITED_INFORMATION. */
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	SetLastError(0);
	if (!event || SetThreadIdealProcessor(event, 0) != (DWORD)-1 || GetLastError() != ERROR_INVALID_HANDLE)
		return 23;
	CloseHandle(event);
	HANDLE query_only = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
	HANDLE set_only = OpenThread(THREAD_SET_INFORMATION, FALSE, GetCurrentThreadId());
	if (!query_only || !set_only)
		return 24;
	SetLastError(0);
	if (SetThreadIdealProcessor(query_only, 0) != (DWORD)-1 || GetLastError() != ERROR_ACCESS_DENIED)
		return 25;
	SetLastError(0);
	if (SetThreadIdealProcessorEx(query_only, &first, NULL) || GetLastError() != ERROR_ACCESS_DENIED)
		return 26;
	SetLastError(0);
	if (GetThreadIdealProcessorEx(set_only, &number) || GetLastError() != ERROR_ACCESS_DENIED)
		return 27;
	if (!GetThreadIdealProcessorEx(query_only, &number) || SetThreadIdealProcessor(set_only, start) != start)
		return 28;
	/* THREAD_QUERY_INFORMATION includes limited-query access even when DuplicateHandle keeps the unexpanded mask. */
	HANDLE duplicate_query;
	if (!DuplicateHandle(GetCurrentProcess(), query_only, GetCurrentProcess(), &duplicate_query, THREAD_QUERY_INFORMATION, FALSE, 0) ||
		!GetThreadIdealProcessorEx(duplicate_query, &number) || number.Group || number.Number != start)
		return 32;
	SetLastError(0);
	if (SetThreadIdealProcessor(duplicate_query, start) != (DWORD)-1 || GetLastError() != ERROR_ACCESS_DENIED)
		return 33;
	CloseHandle(duplicate_query);
	HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
	if (!limited || !GetThreadIdealProcessorEx(limited, &number) || number.Number != start)
		return 29;
	CloseHandle(limited);
	CloseHandle(query_only);
	CloseHandle(set_only);
	return 0;
}
