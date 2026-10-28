#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#define CHECK(n,c) do { if(!(c)) return n; } while(0)
#define MIB (1024ULL*1024)
static int run(void)
{
	MEMORYSTATUSEX before = {sizeof(before)}, during = {sizeof(during)}, after = {sizeof(after)};
	PROCESS_MEMORY_COUNTERS_EX p0 = {sizeof(p0)}, p1 = {sizeof(p1)}, p2 = {sizeof(p2)};
	PERFORMANCE_INFORMATION perf = {sizeof(perf)};
	CHECK(1, GlobalMemoryStatusEx(&before));
	CHECK(2, before.ullTotalVirtual == (sizeof(void*) == 4 ? 0x7ffe0000ULL : 0x7ffffffe0000ULL));
	CHECK(3, before.ullAvailVirtual < before.ullTotalVirtual);
	CHECK(4, before.ullAvailPageFile <= before.ullTotalPageFile);
	CHECK(5, GetPerformanceInfo(&perf, sizeof(perf)));
	CHECK(6, (ULONGLONG)perf.CommitLimit * perf.PageSize == before.ullTotalPageFile);
	CHECK(7, GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p0, sizeof(p0)));
	volatile char *p = VirtualAlloc(0, 640*MIB, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	if (before.ullTotalPageFile == 64*MIB) {
		CHECK(8, !p && GetLastError() == 1455);
		// Small commits in pre-opened granules exercise the guest commit log and its fallback.
		char *r = VirtualAlloc(0, 80*MIB, MEM_RESERVE, PAGE_READWRITE);
		CHECK(9, r != 0);
		SIZE_T committed = 0;
		while (committed < 80*MIB) {
			if (!VirtualAlloc(r+committed, 4096, MEM_COMMIT, PAGE_READWRITE)) break;
			committed += 4096;
		}
		CHECK(10, committed > MIB && committed <= 64*MIB && GetLastError() == 1455);
		CHECK(11, GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p1, sizeof(p1)));
		CHECK(12, p1.PrivateUsage == p0.PrivateUsage + committed);
		CHECK(13, VirtualFree(r, 0, MEM_RELEASE));
		return 0;
	}
	CHECK(14, before.ullTotalPageFile >= before.ullTotalPhys);
	CHECK(15, p != 0);
	for (SIZE_T i=0; i<640*MIB; i+=64*MIB) p[i]=42;
	CHECK(16, GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p1, sizeof(p1)));
	CHECK(17, p1.PrivateUsage == p0.PrivateUsage + 640*MIB);
	CHECK(18, GlobalMemoryStatusEx(&during));
	CHECK(19, before.ullAvailPageFile > during.ullAvailPageFile + 512*MIB);
	CHECK(20, VirtualFree((void*)p, 0, MEM_RELEASE));
	CHECK(21, GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p2, sizeof(p2)));
	CHECK(22, p2.PrivateUsage == p0.PrivateUsage && p2.PeakPagefileUsage >= p1.PagefileUsage);
	CHECK(23, GlobalMemoryStatusEx(&after));
	CHECK(24, after.ullAvailPageFile > during.ullAvailPageFile + 512*MIB);
	return 0;
}
int raven_entry(void) { return run(); }
