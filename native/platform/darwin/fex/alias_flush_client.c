// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#ifndef ALIAS_FLUSH
#define ALIAS_FLUSH 0
#endif
static const WCHAR section_name[] = L"Local\\RavenAliasCode";
typedef LONG (WINAPI *NtFlush)(HANDLE, const void *, SIZE_T);
static unsigned execute(const BYTE *code) { return ((unsigned (*)(void))code)(); }
int raven_entry(void)
{
    const WCHAR *line = GetCommandLineW();
    for (unsigned i = 0; line[i]; ++i) {
        if (line[i] == '-' && line[i+1] == '-') {
            HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, section_name);
            volatile BYTE *rw = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 16384) : NULL;
            if (!rw) return 30;
            rw[1] = 43;
            if (!UnmapViewOfFile((const void *)rw) || !CloseHandle(section)) return 31;
            return 0;
        }
    }
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 16384, section_name);
    BYTE *rw = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 16384) : NULL;
    const BYTE *rx = section ? MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 16384) : NULL;
    HANDLE self = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
    NtFlush nt_flush = (NtFlush)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtFlushInstructionCache");
    if (!rw || !rx || rw == rx || !self || !nt_flush) return 1;
    // Unmapped, overflowing, and partially mapped ranges must all be harmless.
    if (!FlushInstructionCache(self, (void *)(ULONG_PTR)0xdead0000, 4096) ||
        nt_flush(self, (void *)(~(ULONG_PTR)0 - 3), 65536) ||
        !FlushInstructionCache(self, (void *)((ULONG_PTR)rx - 4096), 24576)) return 2;
    SetLastError(0);
    if (FlushInstructionCache((HANDLE)(ULONG_PTR)0xdeadbeef, rx, 6) || GetLastError() != ERROR_INVALID_HANDLE ||
        nt_flush((HANDLE)(ULONG_PTR)0xdeadbeef, rx, 6) != (LONG)0xc0000008) return 3;
    rw[0] = 0xb8; rw[1] = 41; rw[2] = rw[3] = rw[4] = 0; rw[5] = 0xc3;
    if (execute(rx) != 41) return 4;
    ((volatile BYTE *)rw)[1] = 42;
    if (ALIAS_FLUSH && !FlushInstructionCache(self, rx, 6)) return 5;
    if (execute(rx) != 42) return 6;

    WCHAR command[1100];
    command[0] = '"';
    DWORD n = GetModuleFileNameW(NULL, command + 1, 1000);
    if (!n || n >= 1000) return 7;
    const WCHAR suffix[] = L"\" --writer";
    for (unsigned i = 0; i < sizeof(suffix)/sizeof(*suffix); ++i) command[n + 1 + i] = suffix[i];
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child)) return 8;
    // A real remote process handle must succeed even with no remote-flush transport.
    BOOL flushed = FlushInstructionCache(child.hProcess, rx, 6);
    LONG status = nt_flush(child.hProcess, (void *)(ULONG_PTR)0xdead0000, 4096);
    if (ResumeThread(child.hThread) == (DWORD)-1) { TerminateProcess(child.hProcess, 90); return 9; }
    if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 91); return 10; }
    DWORD exit_code = 99;
    if (!GetExitCodeProcess(child.hProcess, &exit_code) || exit_code || !flushed || status) return 11;
    if (ALIAS_FLUSH && nt_flush(self, rx, 6)) return 12;
    if (execute(rx) != 43) return 13;
    if (!CloseHandle(child.hThread) || !CloseHandle(child.hProcess) || !CloseHandle(self) ||
        !UnmapViewOfFile(rw) || !UnmapViewOfFile(rx) || !CloseHandle(section)) return 14;
    return 0;
}
