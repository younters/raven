#include <windows.h>

/* NtQuerySemaphore and ZwQuerySemaphore: SEMAPHORE_BASIC_INFORMATION, the current and the maximum count, and the statuses for a wrong
 * class, a wrong length, a buffer the program cannot write, a handle that is not a semaphore's (an event or a file) and a handle without
 * SEMAPHORE_QUERY_STATE. NtQuerySemaphore has no Microsoft Learn page: the exact length, the statuses and the order of the checks (class,
 * length, handle type, access) are Raven's choice, matching Wine 10.0's dlls/ntdll/unix/sync.c and server/handle.c (consulted for
 * behaviour only). Documented: CreateSemaphore's handle has full access (Synchronization Object Security and Access Rights), and from user
 * mode the Nt and Zw names of a service behave identically (Using Nt and Zw Versions of the Native System Services Routines). */
typedef LONG (WINAPI *QuerySemaphore)(HANDLE, int, void *, ULONG, ULONG *);

UINT raven_entry(void)
{
	QuerySemaphore query = (QuerySemaphore)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySemaphore");
	if (!query)
		return 1;
	HANDLE semaphore = CreateSemaphoreW(NULL, 2, 5, NULL);
	if (!semaphore)
		return 2;
	LONG info[2] = {-1, -1};
	ULONG written = 0;
	if (query(semaphore, 0, info, sizeof info, &written) || written != sizeof info || info[0] != 2 || info[1] != 5)
		return 3;
	/* ZwQuerySemaphore is the same service under its Zw name. */
	QuerySemaphore zw_query = (QuerySemaphore)GetProcAddress(GetModuleHandleA("ntdll.dll"), "ZwQuerySemaphore");
	info[0] = info[1] = -1;
	if (!zw_query || zw_query(semaphore, 0, info, sizeof info, NULL) || info[0] != 2 || info[1] != 5)
		return 22;
	if (!ReleaseSemaphore(semaphore, 2, NULL) || query(semaphore, 0, info, sizeof info, NULL) || info[0] != 4 || info[1] != 5)
		return 4;
	if (WaitForSingleObject(semaphore, 0) || query(semaphore, 0, info, sizeof info, NULL) || info[0] != 3)
		return 5;
	/* STATUS_INVALID_INFO_CLASS, STATUS_INFO_LENGTH_MISMATCH. */
	if ((ULONG)query(semaphore, 1, info, sizeof info, NULL) != 0xc0000003U)
		return 6;
	if ((ULONG)query(semaphore, 0, info, sizeof info - 1, NULL) != 0xc0000004U ||
		(ULONG)query(semaphore, 0, info, sizeof info + 1, NULL) != 0xc0000004U)
		return 7;
	/* STATUS_ACCESS_VIOLATION for an information buffer or a returned length the program cannot write. */
	if ((ULONG)query(semaphore, 0, (void *)1, sizeof info, NULL) != 0xc0000005U)
		return 8;
	if ((ULONG)query(semaphore, 0, info, sizeof info, (ULONG *)1) != 0xc0000005U)
		return 9;
	/* STATUS_OBJECT_TYPE_MISMATCH for an event, STATUS_INVALID_HANDLE for a closed handle. */
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event || (ULONG)query(event, 0, info, sizeof info, NULL) != 0xc0000024U)
		return 10;
	/* A file handle is a valid handle of another type too: STATUS_OBJECT_TYPE_MISMATCH, not STATUS_INVALID_HANDLE. */
	WCHAR self[MAX_PATH];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH))
		return 23;
	HANDLE file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || (ULONG)query(file, 0, info, sizeof info, NULL) != 0xc0000024U)
		return 24;
	CloseHandle(file);
	/* When several arguments are wrong, the earlier check answers: class over length over handle, type over access. */
	if ((ULONG)query((HANDLE)0xdeadbee0, 1, info, sizeof info - 1, NULL) != 0xc0000003U)
		return 12;
	if ((ULONG)query((HANDLE)0xdeadbee0, 0, info, sizeof info - 1, NULL) != 0xc0000004U)
		return 13;
	HANDLE event_wait = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &event_wait, SYNCHRONIZE, FALSE, 0) ||
		(ULONG)query(event_wait, 0, info, sizeof info, NULL) != 0xc0000024U)
		return 14;
	CloseHandle(event_wait);
	/* A copy holding only SYNCHRONIZE may wait but not query: STATUS_ACCESS_DENIED, with nothing written. Copies with the same access,
	 * SEMAPHORE_QUERY_STATE or GENERIC_READ may query. */
	HANDLE copy = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), semaphore, GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0))
		return 15;
	info[0] = info[1] = -1;
	written = 7;
	if ((ULONG)query(copy, 0, info, sizeof info, &written) != 0xc0000022U || info[0] != -1 || info[1] != -1 || written != 7)
		return 16;
	if ((ULONG)query(copy, 0, info, sizeof info - 1, NULL) != 0xc0000004U)
		return 17;
	if (WaitForSingleObject(copy, 0))
		return 18;
	CloseHandle(copy);
	static const DWORD allowed[][2] = {{0, DUPLICATE_SAME_ACCESS}, {0x0001, 0}, {GENERIC_READ, 0}};
	for (int index = 0; index < 3; ++index) {
		copy = NULL;
		if (!DuplicateHandle(GetCurrentProcess(), semaphore, GetCurrentProcess(), &copy, allowed[index][0], FALSE, allowed[index][1]) ||
			query(copy, 0, info, sizeof info, NULL) || info[0] != 2 || info[1] != 5)
			return 19 + index;
		CloseHandle(copy);
	}
	CloseHandle(event);
	CloseHandle(semaphore);
	if ((ULONG)query(semaphore, 0, info, sizeof info, NULL) != 0xc0000008U)
		return 11;
	return 0;
}
