#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint64_t initial;
	uint32_t shared, local;
	void *private_data;
	uint32_t size, flags;
} CREATE_KEYED;
typedef struct {
	uint32_t shared, local;
	void *private_data;
	uint32_t size;
} OPEN_KEYED;
typedef struct {
	uint32_t local, padding;
	uint64_t key;
	int64_t *timeout;
	uint64_t fence;
} ACQUIRE_KEYED;
typedef struct {
	uint32_t local, padding;
	uint64_t key, fence;
} RELEASE_KEYED;
typedef struct { uint32_t local; } DESTROY_KEYED;

__declspec(dllimport) LONG WINAPI D3DKMTCreateKeyedMutex2(CREATE_KEYED *);
__declspec(dllimport) LONG WINAPI D3DKMTOpenKeyedMutex2(OPEN_KEYED *);
__declspec(dllimport) LONG WINAPI D3DKMTAcquireKeyedMutex(ACQUIRE_KEYED *);
__declspec(dllimport) LONG WINAPI D3DKMTReleaseKeyedMutex(RELEASE_KEYED *);
__declspec(dllimport) LONG WINAPI D3DKMTDestroyKeyedMutex(DESTROY_KEYED *);

_Static_assert(offsetof(ACQUIRE_KEYED, key) == 8, "key offset");
_Static_assert(offsetof(ACQUIRE_KEYED, timeout) == 16, "timeout offset");
_Static_assert(offsetof(ACQUIRE_KEYED, fence) == 24, "fence offset");
_Static_assert(offsetof(RELEASE_KEYED, fence) == 16, "release fence offset");

static void decimal(WCHAR *out, uint32_t value)
{
	WCHAR reversed[16];
	unsigned count = 0;
	do { reversed[count++] = L'0' + value % 10; value /= 10; } while (value);
	while (count) *out++ = reversed[--count];
	*out = 0;
}

static uint32_t number(const WCHAR *text)
{
	uint32_t value = 0;
	while (*text >= L'0' && *text <= L'9') value = value * 10 + *text++ - L'0';
	return value;
}

static LONG acquire(uint32_t local, uint64_t key, int64_t *timeout, uint64_t *fence)
{
	ACQUIRE_KEYED request = {0};
	request.local = local;
	request.key = key;
	request.timeout = timeout;
	LONG status = D3DKMTAcquireKeyedMutex(&request);
	*fence = request.fence;
	return status;
}

static LONG release(uint32_t local, uint64_t key, uint64_t fence)
{
	RELEASE_KEYED request = {0};
	request.local = local;
	request.key = key;
	request.fence = fence;
	return D3DKMTReleaseKeyedMutex(&request);
}

static void result(const char *message)
{
	HANDLE file = CreateFileA("C:\\dxmt-keyed.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD written;
		WriteFile(file, message, lstrlenA(message), &written, NULL);
		CloseHandle(file);
	}
}

static int child(uint32_t shared, int abandon)
{
	char name[54] = {0};
	OPEN_KEYED open = {0};
	open.shared = shared;
	open.private_data = name;
	open.size = sizeof(name);
	if (D3DKMTOpenKeyedMutex2(&open) || !open.local || name[0] != 'R') return 31;
	uint64_t fence = 0;
	int64_t timeout = -50000000; // five seconds in 100 ns units
	if (acquire(open.local, abandon ? 3 : 1, &timeout, &fence) || fence != (abandon ? 3 : 1)) return 32;
	if (abandon) ExitProcess(0); // the file lock must be released by process death
	if (release(open.local, 2, 2)) return 33;
	DESTROY_KEYED destroy = {open.local};
	return D3DKMTDestroyKeyedMutex(&destroy) ? 34 : 0;
}

static DWORD WINAPI contender(void *argument)
{
    uint32_t local = (uint32_t)(uintptr_t)argument;
    int64_t timeout = -50000000;
    uint64_t fence = 0;
    if (acquire(local, 1, &timeout, &fence) || fence != 1) return 41;
    return release(local, 2, 2) ? 42 : 0;
}

static void local_contention(void)
{
    char name[54] = "Raven two-thread scheduling";
    CREATE_KEYED create = {0};
    create.private_data = name; create.size = sizeof(name);
    if (D3DKMTCreateKeyedMutex2(&create)) ExitProcess(40);
    OPEN_KEYED opened = {0};
    opened.shared = create.shared; opened.private_data = name; opened.size = sizeof(name);
    if (D3DKMTOpenKeyedMutex2(&opened)) ExitProcess(40);
    uint64_t fence = 0; int64_t zero = 0;
    if (acquire(create.local, 0, &zero, &fence)) ExitProcess(40);
    HANDLE thread = CreateThread(NULL, 0, contender, (void *)(uintptr_t)opened.local, 0, NULL);
    if (!thread) ExitProcess(40);
    Sleep(20); // The contender must park its guest frame, allowing this thread to release.
    if (release(create.local, 1, 1)) ExitProcess(43);
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) ExitProcess(44);
    DWORD code = 99;
    if (!GetExitCodeThread(thread, &code) || code) ExitProcess(code ? code : 45);
    CloseHandle(thread);
    DESTROY_KEYED destroy = {opened.local};
    if (D3DKMTDestroyKeyedMutex(&destroy)) ExitProcess(45);
    destroy.local = create.local;
    if (D3DKMTDestroyKeyedMutex(&destroy)) ExitProcess(45);
}

void entry(void)
{
	WCHAR *command = GetCommandLineW();
	for (; *command && *command != L' '; ++command) {}
	if (*command) {
		while (*command == L' ') ++command;
		int abandon = *command == L'a';
		while (*command && *command != L' ') ++command;
		while (*command == L' ') ++command;
		ExitProcess(child(number(command), abandon));
	}
	local_contention();
	char name[54] = "Raven keyed mutex cross process";
	CREATE_KEYED create = {0};
	create.private_data = name;
	create.size = sizeof(name);
	if (D3DKMTCreateKeyedMutex2(&create) || !create.local || !create.shared) ExitProcess(10);
	uint64_t fence = 99;
	int64_t zero = 0;
	if (acquire(create.local, 0, &zero, &fence) || fence != 0) ExitProcess(11);
	WCHAR path[MAX_PATH], line[MAX_PATH + 32], digits[16];
	if (!GetModuleFileNameW(NULL, path, MAX_PATH)) ExitProcess(12);
	lstrcpyW(line, path);
	lstrcatW(line, L" child ");
	decimal(digits, create.shared);
	lstrcatW(line, digits);
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	if (!CreateProcessW(path, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) ExitProcess(13);
	if (release(create.local, 1, 1)) ExitProcess(14);
	if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) ExitProcess(15);
	DWORD code = 99;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	if (code) ExitProcess(code);
	int64_t short_wait = -500000; // 50 ms
	if (acquire(create.local, 99, &short_wait, &fence) != 0x102) ExitProcess(16);
	if (acquire(create.local, 2, &zero, &fence) || fence != 2) ExitProcess(17);
	lstrcpyW(line, path);
	lstrcatW(line, L" abandon ");
	lstrcatW(line, digits);
	if (!CreateProcessW(path, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) ExitProcess(18);
	if (release(create.local, 3, 3)) ExitProcess(19);
	if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) ExitProcess(20);
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	if (code) ExitProcess(code);
	if (acquire(create.local, 4, &zero, &fence) != 0x80) ExitProcess(21);
	if (acquire(create.local, 3, &zero, &fence) != 0x80) ExitProcess(22);
	DESTROY_KEYED destroy = {create.local};
	if (D3DKMTDestroyKeyedMutex(&destroy)) ExitProcess(23);
	result("OK: keyed mutex two-thread wait, two-process handoff, timeout and abandonment");
	ExitProcess(0);
}
