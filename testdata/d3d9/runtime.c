/* Width-sensitive runtime contracts used by the mtld3d PE startup. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
typedef void *(WINAPI *alloc2_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, DWORD);
struct requirements { void *low, *high; SIZE_T alignment; };
struct parameter { uint64_t type, value; };
int raven_entry(void)
{
    alloc2_fn allocate = (alloc2_fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2FromApp");
    if (!allocate) return 1;
    struct requirements req = {0, 0, 2 * 1024 * 1024};
    struct parameter param = {1, (uintptr_t)&req};
    unsigned char *p = allocate(NULL, NULL, 65536, MEM_RESERVE, PAGE_READWRITE, &param, 1);
    if (!p || ((uintptr_t)p & (req.alignment - 1))) return 2;
    if (VirtualAlloc(p, 4096, MEM_COMMIT, PAGE_READWRITE) != p) return 3;
    p[0] = 17; p[4095] = 39;
    if (p[0] != 17 || p[4095] != 39) return 4;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p + 4096, &info, sizeof info) || info.State != MEM_RESERVE) return 5;
    if (!VirtualFree(p, 0, MEM_RELEASE)) return 6;
    p = allocate(NULL, NULL, 4096, MEM_COMMIT, PAGE_READWRITE, &param, 1);
    if (!p || ((uintptr_t)p & (req.alignment - 1))) return 13;
    p[0] = 42;
    if (p[0] != 42 || !VirtualFree(p, 0, MEM_RELEASE)) return 14;
    req.alignment = 98304;
    if (allocate(NULL, NULL, 65536, MEM_RESERVE, PAGE_READWRITE, &param, 1) || GetLastError() != ERROR_INVALID_PARAMETER) return 7;
    if (allocate(NULL, NULL, 65536, MEM_RESERVE, PAGE_EXECUTE_READ, NULL, 0)) return 8;
    p = allocate(NULL, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    if (!p || !VirtualFree(p, 0, MEM_RELEASE)) return 9;
    struct { CURSORINFO info; DWORD canary; } cursor = {{0}, 0x9a4bf103};
    if (GetCursorInfo(&cursor.info) || GetLastError() != ERROR_INVALID_PARAMETER) return 10;
    cursor.info.cbSize = sizeof cursor.info;
    if (!GetCursorInfo(&cursor.info) || cursor.canary != 0x9a4bf103) return 11;
    POINT point;
    if (!GetCursorPos(&point) || cursor.info.ptScreenPos.x != point.x || cursor.info.ptScreenPos.y != point.y) return 12;
    return 0;
}
static int (*volatile const entry_relocation)(void) __attribute__((used)) = raven_entry;
