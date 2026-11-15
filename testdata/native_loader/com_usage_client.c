/* CoIncrementMTAUsage keeps the multithreaded apartment alive, and puts a thread that is in no apartment into it implicitly, until
 * every cookie is returned. Returns 0, or the number of the first failing check. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

typedef HRESULT (WINAPI *increment_usage)(void **);
typedef HRESULT (WINAPI *decrement_usage)(void *);
typedef HRESULT (WINAPI *apartment_type)(DWORD *, DWORD *);

int raven_entry(void)
{
	HMODULE ole = GetModuleHandleW(L"ole32.dll");
	increment_usage increment = (increment_usage)GetProcAddress(ole, "CoIncrementMTAUsage");
	decrement_usage decrement = (decrement_usage)GetProcAddress(ole, "CoDecrementMTAUsage");
	apartment_type apartment = (apartment_type)GetProcAddress(ole, "CoGetApartmentType");
	void *first = NULL, *second = NULL;
	DWORD type, qualifier;
	if (!increment || !decrement || !apartment)
		return 1;
	/* No apartment yet; a null cookie pointer is refused (Raven's choice). */
	if (apartment(&type, &qualifier) != CO_E_NOTINITIALIZED || increment(NULL) != E_INVALIDARG)
		return 2;
	/* Two cookies, each its own, and the thread is in the MTA implicitly (APTTYPE_MTA, APTTYPEQUALIFIER_IMPLICIT_MTA). */
	if (increment(&first) != S_OK || !first || increment(&second) != S_OK || !second || first == second)
		return 3;
	if (apartment(&type, &qualifier) != S_OK || type != APTTYPE_MTA || qualifier != APTTYPEQUALIFIER_IMPLICIT_MTA)
		return 4;
	/* The implicit MTA is no apartment of the thread's own, so it can still enter a single-threaded one (Raven's choice), and is back in
	 * the MTA after. */
	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK || apartment(&type, &qualifier) != S_OK || type != APTTYPE_MAINSTA)
		return 5;
	CoUninitialize();
	if (apartment(&type, &qualifier) != S_OK || type != APTTYPE_MTA || qualifier != APTTYPEQUALIFIER_IMPLICIT_MTA)
		return 6;
	/* A cookie is returned once (a second return is E_INVALIDARG, Raven's choice); the other still keeps the MTA. */
	if (decrement(first) != S_OK || decrement(first) != E_INVALIDARG || decrement((void *)0x7f) != E_INVALIDARG)
		return 7;
	if (apartment(&type, &qualifier) != S_OK || type != APTTYPE_MTA || qualifier != APTTYPEQUALIFIER_IMPLICIT_MTA)
		return 8;
	/* With the last cookie back and no thread in the MTA, the thread is in no apartment again. */
	if (decrement(second) != S_OK || apartment(&type, &qualifier) != CO_E_NOTINITIALIZED)
		return 9;
	/* A cookie does not end the MTA a thread entered itself. */
	if (CoInitializeEx(NULL, COINIT_MULTITHREADED) != S_OK || increment(&first) != S_OK || decrement(first) != S_OK ||
		apartment(&type, &qualifier) != S_OK || type != APTTYPE_MTA || qualifier != APTTYPEQUALIFIER_NONE)
		return 10;
	CoUninitialize();
	return 0;
}
