#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char line[1024];

static char *number(char *out, DWORD value)
{
    char digits[12]; unsigned count=0;
    do { digits[count++]='0'+value%10; value/=10; } while (value);
    while (count) *out++=digits[--count];
    *out=0; return out;
}

static DWORD parse(const char **at)
{
    DWORD value=0;
    while (**at==' ') ++*at;
    while (**at>='0' && **at<='9') {value=value*10+**at-'0'; ++*at;}
    return value;
}

static void report(const char *tag, DWORD mode)
{
    DWORD written; char *out=line;
    while (*tag) *out++=*tag++;
    *out++=' '; out=number(out,mode); *out++=' '; out=number(out,GetCurrentProcessId());
    *out++='\n'; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,(DWORD)(out-line),&written,NULL);
}

int raven_entry(void)
{
    const char *command=GetCommandLineA();
    const char *flag=command;
    while (*flag && !(*flag=='-' && flag[1]=='c' && flag[2]==' ')) ++flag;
    if (*flag) {
        DWORD mode, parent, inherited, bytes=0;
        HANDLE pipe, process;
        BOOL ok;
        flag+=3; mode=parse(&flag); parent=parse(&flag); inherited=parse(&flag);
        pipe=(HANDLE)(ULONG_PTR)inherited;
        process=OpenProcess(SYNCHRONIZE,FALSE,parent);
        if (!process) return 20;
        if (mode==0) {
            char byte; report("READY",mode);
            ok=ReadFile(pipe,&byte,1,&bytes,NULL);
            if (ok || GetLastError()!=ERROR_BROKEN_PIPE) return 21;
            ok=PeekNamedPipe(pipe,NULL,0,NULL,&bytes,NULL);
            if (ok || GetLastError()!=ERROR_BROKEN_PIPE) return 22;
        } else if (mode==1) {
            report("READY",mode);
            if (WaitForSingleObject(process,5000)!=WAIT_OBJECT_0) return 23;
        } else if (mode==2) {
            char byte; OVERLAPPED operation={0}; DWORD transferred=0; ULONG_PTR key=0; OVERLAPPED *done=NULL;
            HANDLE port=CreateIoCompletionPort(pipe,NULL,7,1);
            if (!port) return 24;
            ok=ReadFile(pipe,&byte,1,NULL,&operation);
            if (ok || GetLastError()!=ERROR_IO_PENDING) return 25;
            report("READY",mode);
            ok=GetQueuedCompletionStatus(port,&transferred,&key,&done,5000);
            if (ok || GetLastError()!=ERROR_BROKEN_PIPE || done!=&operation || key!=7 || transferred) return 26;
            CloseHandle(port);
        } else if (mode==3) {
            report("READY",mode);
            Sleep(2500);
        } else return 27;
        CloseHandle(process);
        if (mode!=3) CloseHandle(pipe);
        report("PASS",mode);
        return 0;
    }
    flag=command;
    while (*flag && !(*flag=='-' && flag[1]=='p' && flag[2]==' ')) ++flag;
    if (!*flag) return 1;
    flag+=3;
    DWORD mode=parse(&flag);
    HANDLE server=INVALID_HANDLE_VALUE, client=INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),NULL,TRUE};
    if (mode!=1 && mode!=3) {
        char name[100]="\\\\.\\pipe\\raven-orphan-";
        char *end=name; while (*end) ++end; number(end,GetCurrentProcessId());
        server=CreateNamedPipeA(name,PIPE_ACCESS_OUTBOUND,PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
        if (server==INVALID_HANDLE_VALUE) return 2;
        client=CreateFileA(name,GENERIC_READ,0,&attributes,OPEN_EXISTING,mode==2?FILE_FLAG_OVERLAPPED:0,NULL);
        if (client==INVALID_HANDLE_VALUE) return 3;
        if (!ConnectNamedPipe(server,NULL) && GetLastError()!=ERROR_PIPE_CONNECTED) return 4;
    }
    char image[MAX_PATH]; DWORD len=GetModuleFileNameA(NULL,image,sizeof(image));
    if (!len || len>=sizeof(image)-40) return 5;
    char *out=line; *out++='"';
    for (DWORD i=0;i<len;++i) *out++=image[i];
    *out++='"'; *out++=' '; *out++='-'; *out++='c'; *out++=' ';
    out=number(out,mode); *out++=' '; out=number(out,GetCurrentProcessId());
    *out++=' '; out=number(out,(DWORD)(ULONG_PTR)client);
    STARTUPINFOA startup={0}; PROCESS_INFORMATION child={0}; startup.cb=sizeof(startup);
    if (!CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&child)) return 6;
    if (client!=INVALID_HANDLE_VALUE) CloseHandle(client);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    Sleep(10000);
    if (server!=INVALID_HANDLE_VALUE) CloseHandle(server);
    return 7;
}
