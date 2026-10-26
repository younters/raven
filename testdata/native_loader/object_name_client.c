#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef LONG (WINAPI *query_object_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (WINAPI *create_section_fn)(HANDLE *, ULONG, void *, LARGE_INTEGER *, ULONG, ULONG, HANDLE);

typedef struct {
	USHORT length;
	USHORT capacity;
	WCHAR *buffer;
} object_name;

static int equals(const WCHAR *actual, const WCHAR *expected, unsigned length)
{
	for (unsigned index = 0; index < length; ++index)
		if (actual[index] != expected[index]) return 0;
	return actual[length] == 0;
}

int raven_entry(void)
{
	const WCHAR path[] = L"C:\\nt-object-name-test.txt";
	const WCHAR nt_path[] = L"\\Device\\HarddiskVolume1\\nt-object-name-test.txt";
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE) return 1;
	query_object_fn query = (query_object_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
	if (!query) return 2;
	ULONG needed = 0;
	if ((ULONG)query(file, 1, 0, 0, &needed) != 0xc0000004UL || needed <= sizeof(object_name)) return 3;
	BYTE storage[512];
	if (query(file, 1, storage, sizeof(storage), &needed) != 0) return 4;
	object_name *name = (object_name *)storage;
	if (name->length != sizeof(nt_path) - sizeof(WCHAR) || name->capacity < name->length + sizeof(WCHAR)) return 5;
	if ((BYTE *)name->buffer < storage + sizeof(object_name) ||
	    (BYTE *)name->buffer + name->capacity > storage + sizeof(storage)) return 6;
	if (!equals(name->buffer, nt_path, name->length / sizeof(WCHAR))) return 7;
	if (!CloseHandle(file) || !DeleteFileW(path)) return 8;
	// Chromium's signed-DLL policy opens the image with FILE_EXECUTE, obtains this name, then creates a SEC_IMAGE section.
	WCHAR image_path[512];
	if (!GetModuleFileNameW(0, image_path, 512)) return 9;
	HANDLE image = CreateFileW(image_path, FILE_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (image == INVALID_HANDLE_VALUE) return 10;
	if (query(image, 1, storage, sizeof(storage), &needed) != 0) return 11;
	name = (object_name *)storage;
	if (!name->length || !name->buffer || name->buffer[0] != L'\\') return 12;
	create_section_fn create_section = (create_section_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateSection");
	if (!create_section) return 13;
	HANDLE section = 0;
	if (create_section(&section, 0xf, 0, 0, PAGE_EXECUTE, SEC_IMAGE, image) != 0 || !section) return 14;
	if (!CloseHandle(section) || !CloseHandle(image)) return 15;
	return 0;
}
