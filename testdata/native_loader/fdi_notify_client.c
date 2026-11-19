#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fdi.h>

static int notifications, bad;
static const char token[] = "notification context";
static void * DIAMONDAPI allocate(ULONG size) { return HeapAlloc(GetProcessHeap(), 0, size); }
static void DIAMONDAPI release(void *memory) { HeapFree(GetProcessHeap(), 0, memory); }
static INT_PTR DIAMONDAPI open_file(char *name, int flags, int mode)
{
	return (INT_PTR)CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}
static UINT DIAMONDAPI read_file(INT_PTR handle, void *data, UINT size)
{
	DWORD count;
	return ReadFile((HANDLE)handle, data, size, &count, NULL) ? count : (UINT)-1;
}
static UINT DIAMONDAPI write_file(INT_PTR handle, void *data, UINT size) { return (UINT)-1; }
static int DIAMONDAPI close_file(INT_PTR handle) { return CloseHandle((HANDLE)handle) ? 0 : -1; }
static LONG DIAMONDAPI seek_file(INT_PTR handle, LONG offset, int origin) { return SetFilePointer((HANDLE)handle, offset, NULL, origin); }
static INT_PTR DIAMONDAPI notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info)
{
	if (info->pv != token) { bad = 1; return -1; }
	if (type == fdintCABINET_INFO) {
		if (info->setID != 7 || info->iCabinet || !info->psz1 || info->psz1[0]) { bad = 2; return -1; }
		++notifications;
	} else if (type == fdintCOPY_FILE) {
		if (info->cb != 1 || lstrcmpA(info->psz1, "one")) { bad = 3; return -1; }
		++notifications;
	}
	return 0; /* Skip the file; this fixture checks the public notification layout. */
}

int raven_entry(void)
{
	static const BYTE cabinet[73] = {
		'M','S','C','F', 0,0,0,0, 73,0,0,0, 0,0,0,0, 44,0,0,0, 0,0,0,0,
		3,1, 1,0, 1,0, 0,0, 7,0, 0,0,
		64,0,0,0, 1,0, 0,0,
		1,0,0,0, 0,0,0,0, 0,0, 0,0, 0,0, 0,0, 'o','n','e',0,
		0,0,0,0, 1,0, 1,0, 'x'
	};
	DWORD written;
	HANDLE file = CreateFileA("fdi-notify.cab", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, cabinet, sizeof(cabinet), &written, NULL) || written != sizeof(cabinet)) return 1;
	CloseHandle(file);
	ERF error = {0};
	HFDI fdi = FDICreate(allocate, release, open_file, read_file, write_file, close_file, seek_file, cpuUNKNOWN, &error);
	if (!fdi) return 2;
	BOOL copied = FDICopy(fdi, "fdi-notify.cab", "", 0, notify, NULL, (void *)token);
	FDIDestroy(fdi);
	DeleteFileA("fdi-notify.cab");
	return copied && !bad && notifications == 2 ? 0 : 10 + bad;
}
