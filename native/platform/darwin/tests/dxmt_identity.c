#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <dxgi.h>

typedef struct { USHORT length, maximum; WCHAR *buffer; } COUNTED_NAME;
typedef LONG (WINAPI *dll_handle)(WCHAR *, ULONG *, COUNTED_NAME *, HMODULE *);
typedef void *(WINAPI *export_address)(HMODULE, const char *);

void entry(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"), kernel = GetModuleHandleW(L"kernel32.dll"), loaded = NULL;
    dll_handle lookup = (dll_handle)GetProcAddress(nt, "LdrGetDllHandle");
    export_address resolve = (export_address)GetProcAddress(nt, "RtlFindExportedRoutineByName");
    WCHAR counted[8] = {L'K',L'E',L'R',L'N',L'E',L'L',L'3',L'2'}; // deliberately no terminator
    COUNTED_NAME name = {sizeof(counted), sizeof(counted), counted};
    if (!lookup || !resolve) ExitProcess(21);
    SetLastError(4321);
    if (lookup(NULL, NULL, &name, &loaded) || loaded != kernel || GetLastError() != 4321) ExitProcess(22);
    if (resolve(kernel, "GetLastError") != (void *)GetProcAddress(kernel, "GetLastError")) ExitProcess(23);
    SetLastError(4321);
    if (resolve(kernel, "RavenMissingExport") || GetLastError() != 4321) ExitProcess(24);
    name.length = 3;
    if (lookup(NULL, NULL, &name, &loaded) != (LONG)0xc000000d) ExitProcess(25);

    DISPLAY_DEVICEW display = {0};
    display.cb = sizeof(display);
    if (!EnumDisplayDevicesW(NULL, 0, &display, 0)) ExitProcess(10);
    if (lstrcmpW(display.DeviceID, L"PCI\\VEN_1002&DEV_7340&SUBSYS_00000000")) ExitProcess(11);
    HDEVINFO devices = SetupDiGetClassDevsW(NULL, display.DeviceID, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA info = {0}; info.cbSize = sizeof(info);
    if (!SetupDiEnumDeviceInfo(devices, 0, &info)) ExitProcess(12);
    WCHAR driver[128]; DWORD type = 0, required = 0;
    if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DRIVER, &type, NULL, 0, &required) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !required || type != REG_SZ) ExitProcess(13);
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DRIVER, &type, (BYTE *)driver, sizeof(driver), &required)) ExitProcess(14);
    WCHAR path[256] = L"System\\CurrentControlSet\\Control\\Class\\";
    lstrcatW(path, driver);
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key)) ExitProcess(15);
    WCHAR version[64]; DWORD bytes = sizeof(version);
    if (RegQueryValueExW(key, L"DriverVersion", NULL, &type, (BYTE *)version, &bytes) ||
        type != REG_SZ || lstrcmpW(version, L"31.0.22000.11000")) ExitProcess(16);
    RegCloseKey(key); SetupDiDestroyDeviceInfoList(devices);
    IDXGIFactory *factory = NULL; IDXGIAdapter *adapter = NULL; DXGI_ADAPTER_DESC desc;
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", NULL, 0x800); // system-directory-only lookup
    typedef HRESULT (WINAPI *create_factory)(REFIID, void **);
    create_factory create = dxgi ? (create_factory)GetProcAddress(dxgi, "CreateDXGIFactory") : NULL;
    if (!create || FAILED(create(&IID_IDXGIFactory, (void **)&factory)) ||
        FAILED(IDXGIFactory_EnumAdapters(factory, 0, &adapter)) ||
        FAILED(IDXGIAdapter_GetDesc(adapter, &desc)) || desc.VendorId != 0x1002 || desc.DeviceId != 0x7340) ExitProcess(17);
    IDXGIAdapter_Release(adapter); IDXGIFactory_Release(factory);
    HANDLE file = CreateFileA("C:\\identity.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    const char text[] = "OK: display, SetupAPI, registry and DXGI identity agree";
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof(text)-1, &written, NULL)) ExitProcess(18);
    CloseHandle(file); ExitProcess(0);
}
