#include <windows.h>
#include <stdint.h>

void entry(void)
{
	HMODULE module = LoadLibraryW(L"winemetal.dll");
	if (!module) ExitProcess(1);
	typedef uint64_t (*new_pool_fn)(void);
	typedef uint64_t (*new_string_fn)(const char *, uint32_t);
	typedef uint32_t (*get_string_fn)(uint64_t, char *, uint64_t, uint32_t);
	typedef void (*release_fn)(uint64_t);
	new_pool_fn pool = (new_pool_fn)GetProcAddress(module, "NSAutoreleasePool_alloc_init");
	new_string_fn string = (new_string_fn)GetProcAddress(module, "NSString_string");
	get_string_fn get = (get_string_fn)GetProcAddress(module, "NSString_getCString");
	release_fn release = (release_fn)GetProcAddress(module, "NSObject_release");
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
	HANDLE file = CreateFileW(L"C:\\pool.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written;
	WriteFile(file, "OK", 2, &written, NULL);
	CloseHandle(file);
	ExitProcess(0);
}
