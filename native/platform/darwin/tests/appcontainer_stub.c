#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <userenv.h>

int raven_entry(void)
{
	PSID sid=(PSID)(ULONG_PTR)1;
	SetLastError(0);
	HRESULT result=DeriveAppContainerSidFromAppContainerName(L"Chrome.Sandbox", &sid);
	if (result != HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED) || sid ||
		GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) return 1;
	SetLastError(0);
	if (DeriveAppContainerSidFromAppContainerName(NULL, NULL) != HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED) ||
		GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) return 2;
	return 0;
}
