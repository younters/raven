#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <objidl.h>

typedef HRESULT (WINAPI *increment_usage)(void **);
typedef HRESULT (WINAPI *decrement_usage)(void *);
typedef HRESULT (WINAPI *apartment_type)(DWORD *, DWORD *);
static const CLSID global_options_class = {0x0000034b, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

int raven_entry(void)
{
	HMODULE ole = GetModuleHandleW(L"ole32.dll");
	increment_usage increment = (increment_usage)GetProcAddress(ole, "CoIncrementMTAUsage");
	decrement_usage decrement = (decrement_usage)GetProcAddress(ole, "CoDecrementMTAUsage");
	apartment_type apartment = (apartment_type)GetProcAddress(ole, "CoGetApartmentType");
	void *first = NULL, *second = NULL;
	DWORD type, qualifier;
	if (!increment || !decrement || !apartment || increment(NULL) != E_INVALIDARG ||
		apartment(&type, &qualifier) != CO_E_NOTINITIALIZED || increment(&first) || !first ||
		increment(&second) || !second || first == second || apartment(&type, &qualifier) || type != 1 || qualifier != 1) return 1;
	/* A usage cookie is independent of this thread's explicit STA apartment. */
	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) || apartment(&type, &qualifier) || type != 3 || qualifier) return 2;
	CoUninitialize();
	if (decrement(first) || decrement(first) != E_INVALIDARG || apartment(&type, &qualifier) || type != 1 || qualifier != 1 ||
		decrement(second) || apartment(&type, &qualifier) != CO_E_NOTINITIALIZED) return 3;
	if (CoInitializeEx(NULL, COINIT_MULTITHREADED)) return 4;
	IGlobalOptions *options = NULL, *other = NULL;
	ULONG_PTR value = 99;
	if (CoCreateInstance(&global_options_class, NULL, CLSCTX_INPROC_SERVER, &IID_IGlobalOptions, (void **)&options) ||
		IGlobalOptions_Query(options, COMGLB_EXCEPTION_HANDLING, &value) || value != COMGLB_EXCEPTION_HANDLE ||
		IGlobalOptions_Set(options, COMGLB_EXCEPTION_HANDLING, COMGLB_EXCEPTION_DONOT_HANDLE) ||
		CoCreateInstance(&global_options_class, NULL, CLSCTX_INPROC_SERVER, &IID_IGlobalOptions, (void **)&other) ||
		IGlobalOptions_Query(other, COMGLB_EXCEPTION_HANDLING, &value) || value != COMGLB_EXCEPTION_DONOT_HANDLE) return 5;
	if (IGlobalOptions_Set(options, COMGLB_EXCEPTION_HANDLING, 99) != E_INVALIDARG ||
		IGlobalOptions_Query(options, 99, &value) != E_INVALIDARG ||
		IGlobalOptions_Query(options, COMGLB_EXCEPTION_HANDLING, NULL) != E_POINTER) return 6;
	IGlobalOptions_Release(options);
	IGlobalOptions_Release(other);
	CoUninitialize();
	return 0;
}
