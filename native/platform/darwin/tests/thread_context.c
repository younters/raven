#include <windows.h>
static volatile LONG ready;
static DWORD WINAPI target(void *value) { ExitThread((DWORD)(ULONG_PTR)value); return 0; }
__attribute__((naked)) static DWORD WINAPI worker(void *value) {
    __asm__ volatile("movl $1, ready(%rip)\n1: pause\njmp 1b");
}
int raven_entry(void) {
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    CONTEXT registers = {0};
    registers.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!thread) return 1;
    while (!ready) Sleep(1);
    if (SetThreadContext(thread, &registers) || GetLastError() != ERROR_ACCESS_DENIED) return 2;
    if (SuspendThread(thread) != 0 || !GetThreadContext(thread, &registers)) return 3;
    registers.Rip = (DWORD64)target;
    registers.Rcx = 321;
    if (!SetThreadContext(thread, &registers) || ResumeThread(thread) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 4;
    DWORD result;
    if (!GetExitCodeThread(thread, &result) || result != 321) return 5;
    CloseHandle(thread);
    return 0;
}
