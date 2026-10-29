#include <windows.h>

static unsigned decimal(WCHAR *out, DWORD value)
{
	WCHAR digits[16]; unsigned count=0, size=0;
	do { digits[count++]=L'0'+value%10; value/=10; } while (value);
	while (count) out[size++]=digits[--count];
	out[size]=0; return size;
}

static void name(WCHAR *out, const WCHAR *prefix, DWORD pid)
{
	unsigned at=0; while (prefix[at]) {out[at]=prefix[at];++at;} decimal(out+at,pid);
}

int raven_entry(void)
{
	DWORD parent=0; const WCHAR *line=GetCommandLineW();
	for (unsigned i=0;line[i];++i) if (line[i]==L'-' && line[i+1]==L'c' && line[i+2]==L' ') {
		unsigned at=i+3; while (line[at]>=L'0' && line[at]<=L'9') parent=parent*10+line[at++]-L'0'; break;
	}
	WCHAR pipe_name[100], event_name[100];
	name(pipe_name,L"\\\\.\\pipe\\raven-semaphore-transfer-",parent ? parent : GetCurrentProcessId());
	name(event_name,L"Local\\raven-semaphore-transfer-",parent ? parent : GetCurrentProcessId());
	if (parent) {
		HANDLE semaphore=CreateSemaphoreW(NULL,1,2,NULL), alias=NULL;
		if (!semaphore || !DuplicateHandle(GetCurrentProcess(),semaphore,GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS)) return 1;
		HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,event_name);
		HANDLE pipe=CreateFileW(pipe_name,GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
		DWORD bytes=0;
		if (!ready || pipe==INVALID_HANDLE_VALUE || !WriteFile(pipe,&semaphore,sizeof(semaphore),&bytes,NULL) || bytes!=sizeof(semaphore)) return 2;
		CloseHandle(pipe);
		if (WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) return 3;
		/* The parent fetched and closed the original handle, consuming its count. */
		LONG previous=-1;
		if (ReleaseSemaphore(semaphore,1,NULL) || GetLastError()!=ERROR_INVALID_HANDLE) return 4;
		if (!ReleaseSemaphore(alias,2,&previous) || previous!=0) return 5;
		if (ReleaseSemaphore(alias,1,NULL) || GetLastError()!=ERROR_TOO_MANY_POSTS) return 6;
		CloseHandle(alias);CloseHandle(ready);return 0;
	}
	HANDLE ready=CreateEventW(NULL,TRUE,FALSE,event_name);
	HANDLE pipe=CreateNamedPipeW(pipe_name,PIPE_ACCESS_INBOUND,PIPE_TYPE_BYTE|PIPE_WAIT,1,256,256,0,NULL);
	if (!ready || pipe==INVALID_HANDLE_VALUE) return 10;
	WCHAR command[1024];unsigned at=0;command[at++]=L'"';at+=GetModuleFileNameW(NULL,command+at,900);command[at++]=L'"';
	command[at++]=L' ';command[at++]=L'-';command[at++]=L'c';command[at++]=L' ';decimal(command+at,GetCurrentProcessId());
	STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION process={0};
	if (!CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process)) return 11;
	if (!ConnectNamedPipe(pipe,NULL) && GetLastError()!=ERROR_PIPE_CONNECTED) return 12;
	HANDLE remote=NULL, semaphore=NULL;DWORD bytes=0;
	if (!ReadFile(pipe,&remote,sizeof(remote),&bytes,NULL) || bytes!=sizeof(remote)) return 13;
	if (!DuplicateHandle(process.hProcess,remote,GetCurrentProcess(),&semaphore,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE)) return 14;
	if (WaitForSingleObject(semaphore,0)!=WAIT_OBJECT_0 || WaitForSingleObject(semaphore,0)!=WAIT_TIMEOUT) return 15;
	if (!SetEvent(ready) || WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0) return 16;
	DWORD code=99;if (!GetExitCodeProcess(process.hProcess,&code) || code) return 20+code;
	if (WaitForSingleObject(semaphore,0)!=WAIT_OBJECT_0 || WaitForSingleObject(semaphore,0)!=WAIT_OBJECT_0 || WaitForSingleObject(semaphore,0)!=WAIT_TIMEOUT) return 17;
	CloseHandle(semaphore);CloseHandle(pipe);CloseHandle(ready);CloseHandle(process.hThread);CloseHandle(process.hProcess);
	return 0;
}
