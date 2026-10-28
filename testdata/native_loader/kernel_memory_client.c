#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#define CHECK(n,c) do { if (!(c)) return n; } while (0)
#define MIB (1024ULL*1024)
static ULONGLONG minimum(ULONGLONG a, ULONGLONG b) { return a<b ? a:b; }
static int quota_test(void)
{
	const char *s=GetCommandLineA();
	for (; *s; ++s) if (s[0]=='-' && s[1]=='-' && s[2]=='q') return 1;
	return 0;
}
static int consistent(int quota)
{
	MEMORYSTATUSEX status={sizeof(status)};
	MEMORYSTATUS legacy={sizeof(legacy)};
	PERFORMANCE_INFORMATION perf={sizeof(perf)};
	PROCESS_MEMORY_COUNTERS_EX process={sizeof(process)};
	BYTE info[312];
	typedef LONG (WINAPI *Query)(ULONG, PVOID, ULONG, PULONG);
	Query query=(Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
	CHECK(30, query && query(2,info,sizeof(info),0)==0);
	CHECK(31, GlobalMemoryStatusEx(&status) && GetPerformanceInfo(&perf,sizeof(perf)));
	CHECK(32, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&process,sizeof(process)));
	GlobalMemoryStatus(&legacy);
	CHECK(33, (ULONGLONG)perf.CommitLimit*perf.PageSize==status.ullTotalPageFile);
	CHECK(34, *(ULONG*)(info+48)==perf.CommitTotal && *(ULONG*)(info+52)==perf.CommitLimit);
	CHECK(35, *(ULONG*)(info+56)>=perf.CommitTotal && perf.CommitPeak>=perf.CommitTotal);
	ULONGLONG system=perf.CommitTotal<perf.CommitLimit ? (ULONGLONG)(perf.CommitLimit-perf.CommitTotal)*perf.PageSize : 0;
	ULONGLONG own=quota ? 64*MIB-minimum(64*MIB,process.PrivateUsage) : status.ullTotalPageFile-minimum(status.ullTotalPageFile,process.PrivateUsage);
	// CommitTotal is in pages; the byte report can differ by a partial host page.
	ULONGLONG expected=minimum(system,own);
	CHECK(36, status.ullAvailPageFile<=expected && expected-status.ullAvailPageFile<perf.PageSize);
	ULONGLONG cap=sizeof(void*)==4 ? 0xffffffffULL : ~(ULONGLONG)0;
	CHECK(37, legacy.dwTotalPageFile==minimum(status.ullTotalPageFile,cap));
	CHECK(38, legacy.dwAvailPageFile==minimum(status.ullAvailPageFile,cap));
	CHECK(39, legacy.dwTotalVirtual==minimum(status.ullTotalVirtual,cap) && legacy.dwAvailVirtual==minimum(status.ullAvailVirtual,cap));
	return 0;
}
static HANDLE start_gate, done[2], exit_gate[2];
static char *ranges[2];
static volatile LONG successes;
static DWORD WINAPI commit_worker(LPVOID argument)
{
	SIZE_T id=(SIZE_T)argument;
	if (WaitForSingleObject(start_gate,5000)!=WAIT_OBJECT_0) return 1;
	for (SIZE_T i=0; i<40*MIB; i+=4096) {
		if (!VirtualAlloc(ranges[id]+i,4096,MEM_COMMIT,PAGE_READWRITE)) {
			if (GetLastError()!=1455) return 2;
			break;
		}
		ranges[id][i]=42;
		InterlockedIncrement(&successes);
	}
	if (!SetEvent(done[id]) || WaitForSingleObject(exit_gate[id],30000)!=WAIT_OBJECT_0) return 3;
	return 0;
}
static int concurrent_commits(int quota)
{
	PROCESS_MEMORY_COUNTERS_EX before={sizeof(before)}, after={sizeof(after)}, released={sizeof(released)};
	HANDLE threads[2];
	start_gate=CreateEventW(0,TRUE,FALSE,0);
	CHECK(40, start_gate!=0);
	for (SIZE_T i=0;i<2;++i) {
		done[i]=CreateEventW(0,TRUE,FALSE,0);
		exit_gate[i]=CreateEventW(0,TRUE,FALSE,0);
		CHECK(52, done[i] && exit_gate[i]);
		ranges[i]=VirtualAlloc(0,40*MIB,MEM_RESERVE,PAGE_READWRITE);
		CHECK(41, ranges[i]!=0);
		threads[i]=CreateThread(0,0,commit_worker,(LPVOID)i,0,0);
		CHECK(42, threads[i]!=0);
	}
	CHECK(43, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&before,sizeof(before)));
	CHECK(44, SetEvent(start_gate));
	for (int i=0;i<2;++i) CHECK(45, WaitForSingleObject(done[i],30000)==WAIT_OBJECT_0);
	CHECK(46, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&after,sizeof(after)));
	// Workers remain parked: their stacks and TEBs cannot change this exact delta.
	CHECK(47, after.PrivateUsage==before.PrivateUsage+(ULONGLONG)successes*4096);
	CHECK(48, successes>256 && (!quota || after.PrivateUsage<=64*MIB));
	for (int i=0;i<2;++i) CHECK(49, VirtualFree(ranges[i],0,MEM_RELEASE));
	CHECK(50, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&released,sizeof(released)));
	CHECK(51, after.PrivateUsage==released.PrivateUsage+(ULONGLONG)successes*4096);
	for (int i=0;i<2;++i) {
		DWORD code=99;
		CHECK(53, SetEvent(exit_gate[i]) && WaitForSingleObject(threads[i],30000)==WAIT_OBJECT_0 &&
			GetExitCodeThread(threads[i],&code) && code==0);
		CloseHandle(threads[i]); CloseHandle(done[i]); CloseHandle(exit_gate[i]);
	}
	CloseHandle(start_gate);
	return 0;
}
static int run(void)
{
	int quota=quota_test(), result=consistent(quota);
	if (result) return result;
	MEMORYSTATUSEX before={sizeof(before)};
	PROCESS_MEMORY_COUNTERS_EX p0={sizeof(p0)}, p1={sizeof(p1)}, p2={sizeof(p2)};
	CHECK(1, GlobalMemoryStatusEx(&before));
	CHECK(2, before.ullTotalVirtual==(sizeof(void*)==4 ? 0x7ffe0000ULL : 0x7ffffffe0000ULL));
	CHECK(3, before.ullAvailVirtual<before.ullTotalVirtual);
	CHECK(4, before.ullTotalPageFile>=before.ullTotalPhys && before.ullAvailPageFile<=before.ullTotalPageFile);
	CHECK(7, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&p0,sizeof(p0)));
	volatile char *p=VirtualAlloc(0,640*MIB,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
	if (quota) {
		CHECK(8, !p && GetLastError()==1455);
		char *r=VirtualAlloc(0,80*MIB,MEM_RESERVE,PAGE_READWRITE);
		CHECK(9, r!=0);
		SIZE_T committed=0;
		while (committed<80*MIB) {
			if (!VirtualAlloc(r+committed,4096,MEM_COMMIT,PAGE_READWRITE)) break;
			committed+=4096;
		}
		CHECK(10, committed>MIB && committed<=64*MIB && GetLastError()==1455);
		CHECK(11, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&p1,sizeof(p1)));
		CHECK(12, p1.PrivateUsage==p0.PrivateUsage+committed);
		result=consistent(quota); if (result) return result;
		CHECK(13, VirtualFree(r,0,MEM_RELEASE));
	} else {
		CHECK(15, p!=0);
		for (SIZE_T i=0;i<640*MIB;i+=64*MIB) p[i]=42;
		CHECK(16, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&p1,sizeof(p1)));
		CHECK(17, p1.PrivateUsage==p0.PrivateUsage+640*MIB);
		result=consistent(quota); if (result) return result;
		CHECK(20, VirtualFree((void*)p,0,MEM_RELEASE));
	}
	CHECK(21, GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&p2,sizeof(p2)));
	CHECK(22, p2.PrivateUsage==p0.PrivateUsage && p2.PeakPagefileUsage>=p1.PagefileUsage);
	result=consistent(quota); if (result) return result;
	return concurrent_commits(quota);
}
int raven_entry(void) { return run(); }
