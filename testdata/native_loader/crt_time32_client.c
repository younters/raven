#include <windows.h>

/* The C runtime's forms with a 32-bit time_t, called by name: _gmtime32 and the rest in both widths, and in the 32-bit library also the
   msvcrt.dll names (gmtime, _fstat, _findfirst, _ftime, ...) that programs built against msvcrt.dll import, whose structures carry 32-bit
   times there. Each structure is followed by guard bytes the function must not write. */

typedef struct {
	int second, minute, hour, day, month, year, weekday, yearday, daylight;
} Time;

/* struct _stat32 (36 bytes) and struct _stat32i64 (48 bytes). */
typedef struct {
	unsigned dev;
	unsigned short ino, mode;
	short nlink, uid, gid;
	unsigned rdev;
	long size;
	long access, modify, change;
} Stat32;

typedef struct {
	unsigned dev;
	unsigned short ino, mode;
	short nlink, uid, gid;
	unsigned rdev;
	long long size;
	long access, modify, change;
} Stat32i64;

/* struct __timeb32 (12 bytes) and struct _finddata32_t (280 bytes). */
typedef struct {
	long time;
	unsigned short milliseconds;
	short zone, daylight;
} TimeB32;

typedef struct {
	unsigned attributes;
	long created, accessed, written;
	unsigned long size;
	char name[260];
} Find32;

#define GUARD 0x5a

static int guarded(const void *buffer, size_t size)
{
	const unsigned char *bytes = buffer;
	for (size_t at = 0; at < 16; ++at)
		if (bytes[size + at] != GUARD)
			return 0;
	return 1;
}

static void fill(void *buffer, size_t size)
{
	unsigned char *bytes = buffer;
	for (size_t at = 0; at < size; ++at)
		bytes[at] = GUARD;
}

static int near_now(long seconds, long now) { return seconds > now - 86400 && seconds < now + 86400; }

static HMODULE crt;
static char directory[MAX_PATH], file[MAX_PATH], pattern[MAX_PATH];
static long now;

static int time_forms(const char *gm_name, const char *local_name, const char *make_name, const char *text_name,
	const char *difference_name)
{
	Time *(__cdecl *gm)(const long *) = (void *)GetProcAddress(crt, gm_name);
	Time *(__cdecl *local)(const long *) = (void *)GetProcAddress(crt, local_name);
	long (__cdecl *make)(Time *) = (void *)GetProcAddress(crt, make_name);
	char *(__cdecl *text)(const long *) = (void *)GetProcAddress(crt, text_name);
	double (__cdecl *difference)(long, long) = (void *)GetProcAddress(crt, difference_name);
	if (!gm || !local || !make || !text || !difference) return 1;
	/* The value sits in the low half of a wider slot whose high half would make a 64-bit read a different time. */
	union { long long wide; long narrow; } slot = {0x7fffffff00000000LL};
	slot.narrow = 365L * 86400 + 86400 + 3661; /* 1971-01-02 01:01:01 UTC */
	const Time *broken = gm(&slot.narrow);
	if (!broken || broken->year != 71 || broken->month || broken->day != 2 || broken->hour != 1 || broken->minute != 1 ||
		broken->second != 1 || broken->yearday != 1 || broken->weekday != 6) return 2;
	if (!local(&slot.narrow)) return 3;
	Time copy = *local(&slot.narrow);
	if (make(&copy) != slot.narrow) return 4;
	Time late = {0, 0, 0, 1, 0, 140, 0, 0, -1}; /* 1 January 2040, past the 32-bit range */
	if (make(&late) != -1) return 5;
	const char *line = text(&slot.narrow);
	if (!line || lstrlenA(line) != 25 || line[24] != '\n' || line[3] != ' ') return 6;
	if (difference(10, 3) != 7.0 || difference(-2147483647L - 1, 2147483647L) != -4294967295.0) return 7;
	return 0;
}

static int stat_forms(const char *stat32, const char *stat32i64)
{
	int (__cdecl *open_file)(const char *, int, ...) = (void *)GetProcAddress(crt, "_open");
	int (__cdecl *close_file)(int) = (void *)GetProcAddress(crt, "_close");
	int (__cdecl *narrow)(int, Stat32 *) = (void *)GetProcAddress(crt, stat32);
	int (__cdecl *wide)(int, Stat32i64 *) = (void *)GetProcAddress(crt, stat32i64);
	if (!open_file || !close_file || !narrow || !wide) return 1;
	const int fd = open_file(file, 0x8000);
	if (fd < 0) return 2;
	unsigned char buffer[64];
	fill(buffer, sizeof buffer);
	Stat32 *small = (Stat32 *)buffer;
	if (narrow(fd, small) || small->size != 5 || (small->mode & 0xf000) != 0x8000 || !near_now(small->modify, now) ||
		!guarded(buffer, sizeof(Stat32))) return 3;
	fill(buffer, sizeof buffer);
	Stat32i64 *large = (Stat32i64 *)buffer;
	if (wide(fd, large) || large->size != 5 || !near_now(large->modify, now) || !guarded(buffer, sizeof(Stat32i64))) return 4;
	close_file(fd);
	if (narrow(-1, small) != -1) return 5;
	return 0;
}

static int path_forms(const char *stat32i64, const char *wide_stat32i64)
{
	int (__cdecl *by_name)(const char *, Stat32i64 *) = (void *)GetProcAddress(crt, stat32i64);
	int (__cdecl *by_wide_name)(const WCHAR *, Stat32i64 *) = (void *)GetProcAddress(crt, wide_stat32i64);
	if (!by_name || !by_wide_name) return 1;
	unsigned char buffer[64];
	fill(buffer, sizeof buffer);
	Stat32i64 *out = (Stat32i64 *)buffer;
	if (by_name(file, out) || out->size != 5 || !near_now(out->modify, now) || !guarded(buffer, sizeof *out)) return 2;
	WCHAR wide[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH);
	fill(buffer, sizeof buffer);
	if (by_wide_name(wide, out) || out->size != 5 || !guarded(buffer, sizeof *out)) return 3;
	return 0;
}

static int search_forms(const char *first_name, const char *next_name, const char *now_name)
{
	intptr_t (__cdecl *first)(const char *, Find32 *) = (void *)GetProcAddress(crt, first_name);
	int (__cdecl *next)(intptr_t, Find32 *) = (void *)GetProcAddress(crt, next_name);
	int (__cdecl *close_search)(intptr_t) = (void *)GetProcAddress(crt, "_findclose");
	void (__cdecl *clock_now)(TimeB32 *) = (void *)GetProcAddress(crt, now_name);
	if (!first || !next || !close_search || !clock_now) return 1;
	unsigned char buffer[320];
	fill(buffer, sizeof buffer);
	Find32 *found = (Find32 *)buffer;
	const intptr_t search = first(pattern, found);
	if (search == -1 || found->size != 5 || lstrcmpiA(found->name, "a.txt") || !near_now(found->written, now) ||
		!guarded(buffer, sizeof(Find32))) return 2;
	if (next(search, found) != -1) return 3;
	close_search(search);
	fill(buffer, sizeof buffer);
	TimeB32 *clock = (TimeB32 *)buffer;
	clock_now(clock);
	if (!near_now(clock->time, now) || clock->milliseconds >= 1000 || !guarded(buffer, sizeof(TimeB32))) return 4;
	return 0;
}

int raven_entry(void)
{
	crt = LoadLibraryA("ucrtbase.dll");
	if (!crt) return 1;
	long (__cdecl *current)(long *) = (void *)GetProcAddress(crt, "_time32");
	if (!current || (now = current(NULL)) <= 0) return 2;
	DWORD length = GetTempPathA(MAX_PATH - 40, directory);
	if (!length) return 3;
	lstrcatA(directory, "raven-time32-");
	char digits[12], *at = digits + sizeof digits;
	*--at = 0;
	for (DWORD id = GetCurrentProcessId(); id || at == digits + sizeof digits - 1; id /= 10)
		*--at = (char)('0' + id % 10);
	lstrcatA(directory, at);
	lstrcpyA(file, directory);
	lstrcatA(file, "\\a.txt");
	lstrcpyA(pattern, directory);
	lstrcatA(pattern, "\\*.txt");
	CreateDirectoryA(directory, NULL);
	HANDLE handle = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written = 0;
	if (handle == INVALID_HANDLE_VALUE || !WriteFile(handle, "hello", 5, &written, NULL) || written != 5) return 4;
	CloseHandle(handle);

	int result;
	if ((result = time_forms("_gmtime32", "_localtime32", "_mktime32", "_ctime32", "_difftime32"))) return 10 + result;
	if ((result = stat_forms("_fstat32", "_fstat32i64"))) return 20 + result;
	if ((result = path_forms("_stat32i64", "_wstat32i64"))) return 30 + result;
	if ((result = search_forms("_findfirst32", "_findnext32", "_ftime32"))) return 40 + result;
	{
		long (__cdecl *make_gm)(Time *) = (void *)GetProcAddress(crt, "_mkgmtime32");
		int (__cdecl *gm_s)(Time *, const long *) = (void *)GetProcAddress(crt, "_gmtime32_s");
		int (__cdecl *local_s)(Time *, const long *) = (void *)GetProcAddress(crt, "_localtime32_s");
		int (__cdecl *clock_s)(TimeB32 *) = (void *)GetProcAddress(crt, "_ftime32_s");
		if (!make_gm || !gm_s || !local_s || !clock_s) return 50;
		Time out, epoch = {0, 0, 0, 1, 0, 70, 0, 0, 0}, early = {0, 0, 0, 31, 11, 69, 0, 0, 0};
		long zero = 0;
		if (make_gm(&epoch) != 0 || make_gm(&early) != -1) return 51;
		if (gm_s(&out, &zero) || out.year != 70 || gm_s(&out, NULL) != 22 || out.year != -1) return 52;
		if (local_s(&out, NULL) != 22 || clock_s(NULL) != 22) return 53;
	}
#ifndef _WIN64
	/* msvcrt.dll's names in the 32-bit library take the same 32-bit layouts. */
	HMODULE msvcrt = LoadLibraryA("msvcrt.dll");
	if (!msvcrt) return 60;
	crt = msvcrt;
	if ((result = time_forms("gmtime", "localtime", "mktime", "ctime", "difftime"))) return 60 + result;
	if ((result = stat_forms("_fstat", "_fstati64"))) return 70 + result;
	if ((result = path_forms("_stati64", "_wstati64"))) return 80 + result;
	if ((result = search_forms("_findfirst", "_findnext", "_ftime"))) return 90 + result;
#endif
	DeleteFileA(file);
	RemoveDirectoryA(directory);
	return 0;
}
