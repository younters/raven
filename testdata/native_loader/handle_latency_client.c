#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Serial operations against a child sleeping in the scheduler, with no UI or disk workload.
int raven_entry(void)
{
	WCHAR image[512], command[560];
	const WCHAR *args = GetCommandLineW();
	for (unsigned i = 0; args[i]; ++i)
		if (args[i] == '-' && args[i + 1] == '-') { Sleep(30000); return 0; }
	DWORD n = GetModuleFileNameW(NULL, image, 512);
	if (!n || n >= 512) return 1;
	command[0] = '"';
	for (DWORD i = 0; i < n; ++i) command[i + 1] = image[i];
	const WCHAR suffix[] = L"\" --idle";
	for (unsigned i = 0; i < sizeof(suffix) / sizeof(WCHAR); ++i) command[n + 1 + i] = suffix[i];
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION child;
	startup.cb = sizeof(startup);
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child)) return 2;
	Sleep(1000);
	void *block = VirtualAllocEx(child.hProcess, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	int result = 0;
	if (!block || !event) result = 3;
	for (unsigned i = 0; !result && i < 60; ++i) {
		DWORD value = 1, previous;
		SIZE_T copied;
		HANDLE remote, local;
		Sleep(3);
		if (!ReadProcessMemory(child.hProcess, block, &value, sizeof(value), &copied) || copied != sizeof(value) || value) result = 4;
		Sleep(3);
		if (!VirtualProtectEx(child.hProcess, block, 4096, i % 2 ? PAGE_READWRITE : PAGE_READONLY, &previous)) result = 5;
		Sleep(3);
		if (!DuplicateHandle(GetCurrentProcess(), event, child.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) result = 6;
		else if (!DuplicateHandle(child.hProcess, remote, GetCurrentProcess(), &local, 0, FALSE,
			DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) result = 7;
		else CloseHandle(local);
	}
	if (block) VirtualFreeEx(child.hProcess, block, 0, MEM_RELEASE);
	CloseHandle(event);
	TerminateProcess(child.hProcess, 0);
	WaitForSingleObject(child.hProcess, 5000);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return result;
}
