#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

static const WCHAR pipe_name[] = L"\\\\.\\pipe\\raven-inherited-listener";

static void append(WCHAR *line, DWORD *used, ULONG_PTR value)
{
	line[(*used)++] = ' ';
	for (unsigned digit = 0; digit < sizeof(value) * 2; ++digit)
		line[(*used)++] = L"0123456789abcdef"[(value >> ((sizeof(value) * 2 - digit - 1) * 4)) & 15];
	line[*used] = 0;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (unsigned at = 0; line[at]; ++at) {
		if (line[at] != '-' || line[at + 1] != '-' || line[at + 2] != 'c') continue;
		ULONG_PTR values[4] = {0};
		at += 3;
		for (unsigned index = 0; index < 4; ++index) {
			while (line[at] == ' ') ++at;
			for (unsigned digit = 0; digit < sizeof(ULONG_PTR) * 2; ++digit) {
				WCHAR character = line[at++];
				values[index] = values[index] * 16 + (character <= '9' ? character - '0' : character - 'a' + 10);
			}
		}
		HANDLE server = (HANDLE)values[0], parent = (HANDLE)values[1], standard = (HANDLE)values[2];
		DWORD count, code, flags;
		BYTE name_buffer[256];
		FILE_NAME_INFO *name = (FILE_NAME_INFO *)name_buffer;
		if (!GetFileInformationByHandleEx(server, FileNameInfo, name, sizeof(name_buffer)) ||
			name->FileNameLength != (sizeof(pipe_name) - 9 * sizeof(WCHAR)) || name->FileName[0] != '\\') return 24;
		HANDLE second = CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE, 2, 4096, 4096, 0, NULL);
		/* The inherited maximum is one, so another instance fails as busy rather than trying to bind the name again. */
		if (second != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) return 25;
		char data;
		if (GetProcessId(parent) != values[3] || !GetExitCodeProcess(parent, &code) || code != STILL_ACTIVE ||
			WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || !GetHandleInformation(parent, &flags) || !(flags & HANDLE_FLAG_INHERIT)) return 20;
		if (GetFileType(standard) == FILE_TYPE_UNKNOWN || !WriteFile(standard, "", 0, &count, NULL)) return 21;
		if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) return 22;
		if (!ReadFile(server, &data, 1, &count, NULL) || count != 1 || data != 'x' ||
			!WriteFile(server, "y", 1, &count, NULL) || count != 1) return 23;
		CloseHandle(server);
		CloseHandle(parent);
		CloseHandle(standard);
		return 0;
	}
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	HANDLE server = CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, 1, 4096, 4096, 0, &security);
	HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, TRUE, GetCurrentProcessId());
	HANDLE standard = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE handles[3] = {server, parent, standard};
	STARTUPINFOEXW startup = {0};
	PROCESS_INFORMATION child;
	BYTE storage[512];
	SIZE_T size = sizeof(storage);
	WCHAR command[640];
	DWORD used = 0, count, code;
	startup.StartupInfo.cb = sizeof(startup);
	startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)storage;
	if (server == INVALID_HANDLE_VALUE || !parent || standard == INVALID_HANDLE_VALUE ||
		!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) ||
		!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL)) return 1;
	command[used++] = '"';
	used += GetModuleFileNameW(NULL, command + used, 480);
	command[used++] = '"'; command[used++] = ' '; command[used++] = '-'; command[used++] = '-'; command[used++] = 'c';
	append(command, &used, (ULONG_PTR)server);
	append(command, &used, (ULONG_PTR)parent);
	append(command, &used, (ULONG_PTR)standard);
	append(command, &used, GetCurrentProcessId());
	if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &child)) return 2;
	/* Only the child keeps the listening server alive when the client connects. */
	CloseHandle(server);
	HANDLE client = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	char data;
	if (client == INVALID_HANDLE_VALUE || !WriteFile(client, "x", 1, &count, NULL) || count != 1 ||
		!ReadFile(client, &data, 1, &count, NULL) || count != 1 || data != 'y') return 3;
	if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code) || code) return code ? (int)code : 4;
	CloseHandle(client);
	CloseHandle(parent);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	static const char passed[] = "PASS: inherited listener, process and standard handles\n";
	WriteFile(standard, passed, sizeof(passed) - 1, &count, NULL);
	return 0;
}
