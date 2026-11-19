#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	ULONGLONG memory = 0;
	if (!GetPhysicallyInstalledSystemMemory(&memory) || !memory)
		return 1;
	const DWORD provider = 0x52534d42; /* RSMB */
	DWORD id = 7;
	if (EnumSystemFirmwareTables(provider, NULL, 0) != 4 || EnumSystemFirmwareTables(provider, &id, 4) != 4 || id)
		return 2;
	const UINT size = GetSystemFirmwareTable(provider, 0, NULL, 0);
	BYTE bytes[512];
	if (size < 8 + 0x1b || size > sizeof bytes)
		return 3;
	bytes[0] = 0x55;
	if (GetSystemFirmwareTable(provider, 0, bytes, 1) != size || bytes[0] != 0x55 ||
		GetSystemFirmwareTable(provider, 0, bytes, sizeof bytes) != size)
		return 4;
	/* RawSMBIOSData 3.2, followed by Type 1 with manufacturer and model strings and an omitted serial number. */
	if (bytes[1] != 3 || bytes[2] != 2 || *(DWORD *)(bytes + 4) != size - 8 || bytes[8] != 1 ||
		bytes[9] != 0x1b || bytes[12] != 1 || bytes[13] != 2 || bytes[15])
		return 5;
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64 || system.dwPageSize != 4096 || !system.dwNumberOfProcessors)
		return 6;
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key))
		return 7;
	WCHAR text[128];
	DWORD type = 0, length = sizeof text;
	const LSTATUS status = RegQueryValueExW(key, L"ProcessorNameString", NULL, &type, (BYTE *)text, &length);
	RegCloseKey(key);
	if (status || type != REG_SZ || length < 4 || length > sizeof text || !text[0] || text[length / 2 - 1])
		return 8;
	return 0;
}
