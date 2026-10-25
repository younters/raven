#include <windows.h>
#include <stdint.h>

typedef uint64_t (*new_pool_fn)(void);
typedef uint64_t (*new_string_fn)(const char *, uint32_t);
typedef uint32_t (*get_string_fn)(uint64_t, char *, uint64_t, uint32_t);
typedef void (*release_fn)(uint64_t);
static new_pool_fn pool;
static new_string_fn string;
static get_string_fn get;
static release_fn release;
static HANDLE ready[2], finish, first_done;

static DWORD WINAPI worker(void *argument)
{
	unsigned index = (unsigned)(uintptr_t)argument;
	uint64_t scope = pool();
	uint64_t text = string("pthread autorelease scope", 4);
	SetEvent(ready[index]);
	if (WaitForSingleObject(finish, 5000) != WAIT_OBJECT_0) return 10;
	if (index && WaitForSingleObject(first_done, 5000) != WAIT_OBJECT_0) return 11;
	char buffer[64];
	if (!get(text, buffer, sizeof buffer, 4)) return 12;
	const char expected[] = "pthread autorelease scope";
	for (unsigned i = 0; i < sizeof expected; ++i)
		if (buffer[i] != expected[i]) return 13;
	release(scope);
	if (!index) SetEvent(first_done);
	return 0;
}

void entry(void)
{
	HMODULE module = LoadLibraryW(L"winemetal.dll");
	if (!module) ExitProcess(1);
	pool = (new_pool_fn)GetProcAddress(module, "NSAutoreleasePool_alloc_init");
	string = (new_string_fn)GetProcAddress(module, "NSString_string");
	get = (get_string_fn)GetProcAddress(module, "NSString_getCString");
	release = (release_fn)GetProcAddress(module, "NSObject_release");
	if (!pool || !string || !get || !release) ExitProcess(2);
	uint64_t first = pool(), second = pool();
	uint64_t text = string("overlapping Windows autorelease scopes", 4);
	release(first); // A different Windows thread can finish its older scope while this one is still live.
	char buffer[64];
	if (!get(text, buffer, sizeof buffer, 4)) ExitProcess(3);
	const char expected[] = "overlapping Windows autorelease scopes";
	for (unsigned i = 0; i < sizeof expected; ++i)
		if (buffer[i] != expected[i]) ExitProcess(4);
	release(second);
	ready[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
	ready[1] = CreateEventW(NULL, TRUE, FALSE, NULL);
	finish = CreateEventW(NULL, TRUE, FALSE, NULL);
	first_done = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE workers[2];
	for (unsigned i = 0; i < 2; ++i)
		workers[i] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)i, 0, NULL);
	if (WaitForMultipleObjects(2, ready, TRUE, 5000) != WAIT_OBJECT_0) ExitProcess(5);
	SetEvent(finish);
	if (WaitForMultipleObjects(2, workers, TRUE, 5000) != WAIT_OBJECT_0) ExitProcess(6);
	for (unsigned i = 0; i < 2; ++i) {
		DWORD code;
		if (!GetExitCodeThread(workers[i], &code) || code) ExitProcess(7);
		CloseHandle(workers[i]);
		CloseHandle(ready[i]);
	}
	CloseHandle(finish);
	CloseHandle(first_done);
	HANDLE file = CreateFileW(L"C:\\pool.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	WriteFile(file, "OK", 2, &written, NULL);
	CloseHandle(file);
	ExitProcess(0);
}
