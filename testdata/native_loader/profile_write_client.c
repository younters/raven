#include <windows.h>

UINT raven_entry(void)
{
	static const WCHAR original[] = L"\xfeff[Settings]\r\nCount=3\r\n[Field 1]\r\nType=bitmap\r\n";
	WCHAR directory[MAX_PATH], path[MAX_PATH], value[64];
	BYTE bytes[512];
	DWORD done;
	if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"ini", 0, path)) return 1;
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, original, sizeof(original) - 2, &done, NULL) || done != sizeof(original) - 2) return 2;
	CloseHandle(file);
	if (!WritePrivateProfileStringW(L"Field 1", L"Text", L"Firefox \x03a9", path)) return 3;
	if (!WritePrivateProfileStringW(L"Settings", L"Count", L"4", path)) return 4;
	if (GetPrivateProfileIntW(L"Settings", L"Count", 0, path) != 4) return 5;
	if (GetPrivateProfileStringW(L"Field 1", L"Type", L"", value, 64, path) != 6 || value[0] != 'b') return 6;
	if (GetPrivateProfileStringW(L"Field 1", L"Text", L"", value, 64, path) != 9 || value[8] != 0x03a9) return 7;
	if (!WritePrivateProfileStringW(L"Field 1", L"Text", NULL, path)) return 8;
	if (GetPrivateProfileStringW(L"Field 1", L"Text", L"missing", value, 64, path) != 7) return 9;
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || !ReadFile(file, bytes, sizeof(bytes), &done, NULL) || done < 2 || bytes[0] != 0xff || bytes[1] != 0xfe || done % 2) return 10;
	CloseHandle(file);
	if (!WritePrivateProfileStringW(L"Field 1", NULL, NULL, path)) return 11;
	if (GetPrivateProfileStringW(L"Field 1", L"Type", L"", value, 64, path)) return 12;
	/* A new byte-encoded profile stays byte-encoded, including after replacing a key. */
	DeleteFileW(path);
	if (!WritePrivateProfileStringW(L"Settings", L"Count", L"1", path) || !WritePrivateProfileStringW(L"Settings", L"Count", L"2", path)) return 13;
	if (GetPrivateProfileIntW(L"Settings", L"Count", 0, path) != 2) return 14;
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || !ReadFile(file, bytes, sizeof(bytes), &done, NULL) || done < 2 || bytes[0] != '[' || bytes[1] != 'S') return 15;
	CloseHandle(file);
	DeleteFileW(path);
	return 0;
}
