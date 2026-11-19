#include <windows.h>

static DWORD slot;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        slot = TlsAlloc();
        return slot != TLS_OUT_OF_INDEXES;
    }
    if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH)
        return TlsSetValue(slot, (void *)(ULONG_PTR)GetCurrentThreadId());
    if (reason == DLL_PROCESS_DETACH) TlsFree(slot);
    return TRUE;
}
