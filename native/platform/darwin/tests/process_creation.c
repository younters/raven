#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
    const WCHAR *args = GetCommandLineW();
    for (const WCHAR *p = args; *p; ++p)
        if (p[0] == ' ' && p[1] == '-' && p[2] == 'c') return 37;
    WCHAR path[MAX_PATH], command[MAX_PATH + 16];
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION child = {0};
    DWORD code;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 1;
    WCHAR found[MAX_PATH], *part = NULL;
    DWORD length = SearchPathW(NULL, path, L".exe", MAX_PATH, found, &part);
    if (!length || !part || lstrcmpW(path, found)) return 8;
    if (SearchPathW(NULL, L"C:\\missing-raven-process.exe", NULL, MAX_PATH, found, NULL) ||
        GetLastError() != ERROR_FILE_NOT_FOUND) return 9;
    unsigned n = 0;
    command[n++] = '"';
    for (unsigned i = 0; path[i]; ++i) command[n++] = path[i];
    command[n++] = '"'; command[n++] = ' '; command[n++] = '-'; command[n++] = 'c'; command[n] = 0;
    if (CreateProcessAsUserW((HANDLE)(ULONG_PTR)0x123456, NULL, command, NULL, NULL, FALSE,
                            0, NULL, NULL, &startup, &child) || GetLastError() != ERROR_INVALID_HANDLE) return 2;
    if (!CreateProcessAsUserW(NULL, NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                             NULL, NULL, &startup, &child)) return 3;
    if (!GetExitCodeProcess(child.hProcess, &code) || code != STILL_ACTIVE) return 4;
    if (ResumeThread(child.hThread) != 1) return 5;
    if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0) return 6;
    if (!GetExitCodeProcess(child.hProcess, &code) || code != 37) return 7;
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    return 0;
}
