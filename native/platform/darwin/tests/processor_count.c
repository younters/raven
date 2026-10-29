#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	DWORD count = GetActiveProcessorCount(0);
	if (!count || count != info.dwNumberOfProcessors) return 1;
	if (GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) != count ||
		GetMaximumProcessorCount(0) != count || GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS) != count) return 2;
	SetLastError(0);
	if (GetActiveProcessorCount(1) || GetLastError() != ERROR_INVALID_PARAMETER) return 3;
	SetLastError(0);
	if (GetMaximumProcessorCount(1) || GetLastError() != ERROR_INVALID_PARAMETER) return 4;
	if (*(volatile DWORD *)(ULONG_PTR)0x7ffe03c0 != count) return 5;
	WCHAR number[16];
	if (!GetEnvironmentVariableW(L"NUMBER_OF_PROCESSORS", number, 16)) return 6;
	DWORD env_count=0;
	for (unsigned i=0; number[i]; ++i) env_count=env_count*10+number[i]-L'0';
	if (env_count != count) return 7;
	SYSTEM_INFO native;
	GetNativeSystemInfo(&native);
	if (native.dwNumberOfProcessors != count) return 8;
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION topology[70];
	DWORD size=sizeof(topology), cores=0;
	if (!GetLogicalProcessorInformation(topology, &size)) return 9;
	for (unsigned i=0; i<size/sizeof(topology[0]); ++i)
		if (topology[i].Relationship == RelationProcessorCore) ++cores;
	if (cores != count) return 10;
	BYTE buffer[8192];
	size=sizeof(buffer); cores=0;
	if (!GetLogicalProcessorInformationEx(RelationAll, (void *)buffer, &size)) return 11;
	for (unsigned at=0; at<size;) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *entry=(void *)(buffer+at);
		if (!entry->Size || entry->Size > size-at) return 12;
		if (entry->Relationship == RelationProcessorCore) ++cores;
		if (entry->Relationship == RelationGroup &&
			(entry->Group.GroupInfo[0].ActiveProcessorCount != count || entry->Group.GroupInfo[0].MaximumProcessorCount != count)) return 13;
		at+=entry->Size;
	}
	if (cores != count) return 14;
	DWORD_PTR process_mask, system_mask;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) ||
		process_mask != info.dwActiveProcessorMask || system_mask != process_mask) return 15;
	typedef BOOL (WINAPI *CpuSets)(void *, ULONG, ULONG *, HANDLE, ULONG);
	CpuSets cpu_sets=(CpuSets)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
	ULONG needed=0;
	if (!cpu_sets || !cpu_sets(buffer,sizeof(buffer),&needed,NULL,0)) return 16;
	DWORD sets=0;
	for (unsigned at=0; at<needed;) {
		DWORD record=*(DWORD *)(buffer+at);
		if (record < 16 || record > needed-at || buffer[at+14] != sets) return 17;
		++sets; at+=record;
	}
	if (sets != count) return 18;
	typedef LONG (WINAPI *QueryProcess)(HANDLE, ULONG, void *, ULONG, ULONG *);
	QueryProcess query=(QueryProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
	ULONG_PTR basic[6];
	if (!query || query(GetCurrentProcess(),0,basic,sizeof(basic),NULL) || basic[2] != process_mask) return 19;
	typedef LONG (WINAPI *Power)(ULONG, void *, ULONG, void *, ULONG);
	HMODULE power_module=LoadLibraryW(L"powrprof.dll");
	Power power=power_module ? (Power)GetProcAddress(power_module,"CallNtPowerInformation") : NULL;
	if (!power || power(11,NULL,0,buffer,count*24)) return 20;
	if ((ULONG)power(11,NULL,0,buffer,count*24-1) != 0xc0000023U) return 21;
	return 0;
}
