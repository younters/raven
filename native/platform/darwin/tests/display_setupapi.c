/* The display adapter a configured DXGI identity reports through EnumDisplayDevices is one SetupAPI device,
 * for 32-bit and 64-bit callers alike: SP_DEVINFO_DATA is 28 bytes in the first and 32 in the second. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

/* Zeroing a structure may call memset, and this fixture links no C runtime. */
void *memset(void *target, int value, size_t size)
{
    volatile unsigned char *bytes = target;
    while (size--) *bytes++ = (unsigned char)value;
    return target;
}

void entry(void)
{
    DISPLAY_DEVICEW display = {0};
    display.cb = sizeof(display);
    if (!EnumDisplayDevicesW(NULL, 0, &display, 0)) ExitProcess(10);
    if (lstrcmpW(display.DeviceID, L"PCI\\VEN_1002&DEV_7340&SUBSYS_00000000")) ExitProcess(11);
    HDEVINFO devices = SetupDiGetClassDevsW(NULL, display.DeviceID, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) ExitProcess(12);

    SP_DEVINFO_DATA info = {0};
    if (sizeof(info) != (sizeof(void *) == 8 ? 32 : 28)) ExitProcess(13);
    /* A size that is not this caller's layout is refused before the device is written. */
    info.cbSize = sizeof(void *) == 8 ? 28 : 32;
    SetLastError(0);
    if (SetupDiEnumDeviceInfo(devices, 0, &info) || GetLastError() != ERROR_INVALID_USER_BUFFER || info.DevInst) ExitProcess(14);
    info.cbSize = sizeof(info);
    if (!SetupDiEnumDeviceInfo(devices, 0, &info) || info.cbSize != sizeof(info) || !info.DevInst) ExitProcess(15);
    SP_DEVINFO_DATA next = {0};
    next.cbSize = sizeof(next);
    SetLastError(0);
    if (SetupDiEnumDeviceInfo(devices, 1, &next) || GetLastError() != ERROR_NO_MORE_ITEMS) ExitProcess(16);

    WCHAR driver[128];
    DWORD type = 0, required = 0;
    SetLastError(0);
    if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DRIVER, &type, NULL, 0, &required) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || type != REG_SZ || required != sizeof(L"{4d36e968-e325-11ce-bfc1-08002be10318}\\0000")) ExitProcess(17);
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DRIVER, &type, (BYTE *)driver, sizeof(driver), &required) ||
        lstrcmpiW(driver, L"{4d36e968-e325-11ce-bfc1-08002be10318}\\0000")) ExitProcess(18);
    /* The set is closed by the same side that opened it. */
    if (!SetupDiDestroyDeviceInfoList(devices)) ExitProcess(19);

    /* A device set for another enumerator holds nothing and also opens, walks and closes on one side. */
    HDEVINFO other = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (other == INVALID_HANDLE_VALUE) ExitProcess(20);
    SetLastError(0);
    if (SetupDiEnumDeviceInfo(other, 0, &next) || GetLastError() != ERROR_NO_MORE_ITEMS) ExitProcess(21);
    if (!SetupDiDestroyDeviceInfoList(other)) ExitProcess(22);
    ExitProcess(0);
}
