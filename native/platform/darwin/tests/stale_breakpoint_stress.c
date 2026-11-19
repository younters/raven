#include <windows.h>

#define WORKERS 4
#define ITERATIONS 1000
static volatile LONG hits, entry_hits, finished, bad, stop_load;
volatile LONG entry_executed;
extern DWORD WINAPI entry_thread(void *);
__asm__(".text\n.globl entry_thread\nentry_thread:\nint3\nlock incl entry_executed(%rip)\nxor %eax,%eax\nret\n");
static HANDLE workers[WORKERS];

static LONG CALLBACK breakpoint_handler(EXCEPTION_POINTERS *exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    if (*(const unsigned char *)(ULONG_PTR)exception->ContextRecord->Rip != 0xcc) {
        InterlockedIncrement(&bad);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (exception->ContextRecord->Rip == (DWORD64)(ULONG_PTR)entry_thread) InterlockedIncrement(&entry_hits);
    ++exception->ContextRecord->Rip;
    InterlockedIncrement(&hits);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < ITERATIONS; ++i) {
        __asm__ volatile("int3");
        GetCurrentThreadId();
    }
    InterlockedIncrement(&finished);
    return 0;
}

static DWORD WINAPI short_lived(void *unused) { (void)unused; return GetCurrentThreadId() == 0; }

static DWORD WINAPI load_worker(void *unused)
{
    (void)unused;
    volatile unsigned value = 1;
    while (!stop_load) value = value * 1664525u + 1013904223u;
    return value == 0;
}

int raven_entry(void)
{
    HANDLE load[3], entry[32];
    HMODULE dll = LoadLibraryA("stale-breakpoint-dll.dll");
    if (!dll) return 7;
    if (!AddVectoredExceptionHandler(1, breakpoint_handler)) return 1;
    for (int i = 0; i < 3; ++i) {
        load[i] = CreateThread(NULL, 0, load_worker, NULL, 0, NULL);
        if (!load[i]) return 8;
    }
    for (int i = 0; i < 32; ++i) {
        entry[i] = CreateThread(NULL, 0, entry_thread, NULL, 0, NULL);
        if (!entry[i]) return 9;
    }
    for (int i = 0; i < WORKERS; ++i) {
        workers[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!workers[i]) return 2;
    }
    while (finished != WORKERS) {
        for (int i = 0; i < WORKERS; ++i) {
            CONTEXT context = {0};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(workers[i]) != (DWORD)-1) {
                GetThreadContext(workers[i], &context);
                if (ResumeThread(workers[i]) == (DWORD)-1) InterlockedIncrement(&bad);
            }
        }
        HANDLE thread = CreateThread(NULL, 0, short_lived, NULL, 0, NULL);
        if (!thread) return 3;
        if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 4;
        CloseHandle(thread);
    }
    for (int i = 0; i < WORKERS; ++i) {
        if (WaitForSingleObject(workers[i], 5000) != WAIT_OBJECT_0) return 5;
        CloseHandle(workers[i]);
    }
    for (int i = 0; i < 32; ++i) {
        if (WaitForSingleObject(entry[i], 5000) != WAIT_OBJECT_0) return 10;
        CloseHandle(entry[i]);
    }
    InterlockedExchange(&stop_load, 1);
    for (int i = 0; i < 3; ++i) {
        if (WaitForSingleObject(load[i], 5000) != WAIT_OBJECT_0) return 11;
        CloseHandle(load[i]);
    }
    FreeLibrary(dll);
    // Pack the counts into the failure code so headless runs can inspect them.
    if (bad || entry_hits != 32 || entry_executed != 32 || hits != WORKERS * ITERATIONS + 32)
        return (entry_hits & 63) | ((entry_executed & 63) << 6) | ((hits & 8191) << 12) | ((bad & 63) << 25);
    return 0;
}
