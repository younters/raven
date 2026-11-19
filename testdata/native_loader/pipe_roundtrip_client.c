#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static ULONGLONG samples[10000];
static WCHAR name[96];

static unsigned decimal(WCHAR *out, DWORD value)
{
	WCHAR digits[12]; unsigned size=0;
	do { digits[size++]=L'0'+value%10; value/=10; } while (value);
	for (unsigned i=0;i<size;++i) out[i]=digits[size-i-1];
	out[size]=0; return size;
}

static int complete(HANDLE port, OVERLAPPED *expected)
{
	DWORD bytes=0; ULONG_PTR key=0; OVERLAPPED *operation=NULL;
	return GetQueuedCompletionStatus(port,&bytes,&key,&operation,5000) && bytes==sizeof(DWORD) && operation==expected;
}

static int transfer(HANDLE pipe, HANDLE port, int reading, DWORD *word)
{
	OVERLAPPED operation={0};
	BOOL done=reading ? ReadFile(pipe,word,sizeof(*word),NULL,&operation) : WriteFile(pipe,word,sizeof(*word),NULL,&operation);
	if (!done && GetLastError()!=ERROR_IO_PENDING) return 0;
	return complete(port,&operation);
}

static void print_number(const char *label, ULONGLONG value)
{
	char output[96], digits[24]; unsigned size=0,count=0; DWORD written;
	while (*label) output[size++]=*label++;
	do { digits[count++]='0'+value%10; value/=10; } while (value);
	while (count) output[size++]=digits[--count];
	output[size++]='\n'; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,size,&written,NULL);
}

int raven_entry(void)
{
	const WCHAR *line=GetCommandLineW(); DWORD parent=0, rounds=64; int child=0;
	for (unsigned i=0;line[i];++i) {
		if (line[i]=='-' && line[i+1]=='c' && line[i+2]==' ') {
			child=1; unsigned at=i+3;
			while (line[at]>='0' && line[at]<='9') parent=parent*10+line[at++]-'0';
		}
		if (line[i]=='-' && line[i+1]=='b') rounds=10000;
	}
	const WCHAR prefix[]=L"\\\\.\\pipe\\raven-roundtrip-";
	unsigned used=0; while (prefix[used]) { name[used]=prefix[used]; ++used; }
	decimal(name+used,child ? parent : GetCurrentProcessId());
	HANDLE pipe=child ? CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL) :
		CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
	if (pipe==INVALID_HANDLE_VALUE) return 1;
	HANDLE port=CreateIoCompletionPort(pipe,NULL,1,1);
	if (!port) return 2;
	PROCESS_INFORMATION process={0};
	if (!child) {
		WCHAR command[1024]; STARTUPINFOW startup={0}; startup.cb=sizeof(startup);
		unsigned at=0; command[at++]='"'; at+=GetModuleFileNameW(NULL,command+at,900); command[at++]='"';
		command[at++]=' ';command[at++]='-';command[at++]='c';command[at++]=' ';
		at+=decimal(command+at,GetCurrentProcessId());
		if (rounds==10000) {command[at++]=' ';command[at++]='-';command[at++]='b';command[at]=0;}
		if (!CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process)) return 3;
		OVERLAPPED connected={0};
		BOOL done=ConnectNamedPipe(pipe,&connected);
		DWORD error=done ? 0 : GetLastError();
		if (!done && error!=ERROR_PIPE_CONNECTED && error!=ERROR_IO_PENDING) return 4;
		if (error!=ERROR_PIPE_CONNECTED) {
			DWORD bytes; ULONG_PTR key; OVERLAPPED *operation;
			if (!GetQueuedCompletionStatus(port,&bytes,&key,&operation,5000) || operation!=&connected) return 5;
		}
	}
	LARGE_INTEGER frequency,start,finish; QueryPerformanceFrequency(&frequency);
	for (DWORD i=0;i<rounds;++i) {
		DWORD word=child ? 0 : i;
		QueryPerformanceCounter(&start);
		if (!transfer(pipe,port,child,&word) || word!=i || !transfer(pipe,port,!child,&word) || word!=i) return 6;
		QueryPerformanceCounter(&finish); samples[i]=finish.QuadPart-start.QuadPart;
	}
	if (!CloseHandle(pipe) || !CloseHandle(port)) return 7;
	if (!child) {
		DWORD code;
		if (WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess,&code) || code) return 8;
		CloseHandle(process.hThread);CloseHandle(process.hProcess);
		/* Insertion sort is outside the timed exchanges. */
		for (DWORD i=1;i<rounds;++i) { ULONGLONG value=samples[i];DWORD j=i;while(j && samples[j-1]>value){samples[j]=samples[j-1];--j;}samples[j]=value; }
		print_number("rounds=",rounds);
		print_number("median_us=",samples[rounds/2]*1000000/frequency.QuadPart);
		ULONGLONG p99_us=samples[(rounds*99)/100]*1000000/frequency.QuadPart;
		print_number("p99_us=",p99_us);
		/* Allow scheduling noise while rejecting the multi-second IPC stalls under investigation. */
		if (p99_us>=5000) return 9;
	}
	return 0;
}
