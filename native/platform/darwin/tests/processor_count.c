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
	return 0;
}
