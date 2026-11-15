// A library mapped as an image section, at an address other than the one it prefers, has every base relocation applied: a page holding
// hundreds of pointers, and a pointer that crosses into the next page.
#include <windows.h>

#ifdef RAVEN_RELOCATIONS_LIBRARY
static int targets[16];
#define FOUR(at) &targets[(at) % 16], &targets[((at) + 1) % 16], &targets[((at) + 2) % 16], &targets[((at) + 3) % 16]
#define SIXTEEN(at) FOUR(at), FOUR((at) + 4), FOUR((at) + 8), FOUR((at) + 12)
#define SIXTY_FOUR(at) SIXTEEN(at), SIXTEEN((at) + 16), SIXTEEN((at) + 32), SIXTEEN((at) + 48)
// A full page of pointers, then one that starts two bytes before the end of the page after it.
__declspec(dllexport) __attribute__((aligned(4096))) int *raven_dense[4096 / sizeof(int *)] = {
	SIXTY_FOUR(0), SIXTY_FOUR(1), SIXTY_FOUR(2), SIXTY_FOUR(3), SIXTY_FOUR(4), SIXTY_FOUR(5), SIXTY_FOUR(6), SIXTY_FOUR(7),
#ifndef _WIN64
	SIXTY_FOUR(8), SIXTY_FOUR(9), SIXTY_FOUR(10), SIXTY_FOUR(11), SIXTY_FOUR(12), SIXTY_FOUR(13), SIXTY_FOUR(14), SIXTY_FOUR(15),
#endif
};
struct __attribute__((packed)) Crossing {
	char before[4094];
	int *pointer;
	char after[8];
};
__declspec(dllexport) __attribute__((aligned(4096))) struct Crossing raven_crossing = {{0}, &targets[5], {0}};
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#else
int raven_entry(void)
{
	// The library as the loader starts it, the reference for what each relocated pointer points at.
	HMODULE loaded = LoadLibraryW(L"relocations.dll");
	if (!loaded)
		return 1;
	const BYTE *dense = (const BYTE *)GetProcAddress(loaded, "raven_dense"), *crossing = (const BYTE *)GetProcAddress(loaded, "raven_crossing");
	if (!dense || !crossing)
		return 2;
	WCHAR path[MAX_PATH];
	const DWORD length = GetModuleFileNameW(loaded, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return 3;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 4;
	HANDLE section = CreateFileMappingW(file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
	if (!section)
		return 5;
	const BYTE *view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
	if (!view || view == (const BYTE *)loaded)
		return 6;
	const ULONG_PTR library = (ULONG_PTR)loaded, image = (ULONG_PTR)view;
	const ULONG_PTR *loaded_dense = (const ULONG_PTR *)dense, *mapped_dense = (const ULONG_PTR *)(view + (dense - (const BYTE *)loaded));
	for (unsigned index = 0; index < 4096 / sizeof(ULONG_PTR); ++index)
		if (mapped_dense[index] - image != loaded_dense[index] - library)
			return 7;
	ULONG_PTR loaded_crossing, mapped_crossing;
	const BYTE *mapped = view + (crossing - (const BYTE *)loaded);
	for (unsigned index = 0; index < sizeof(ULONG_PTR); ++index) {
		((BYTE *)&loaded_crossing)[index] = crossing[4094 + index];
		((BYTE *)&mapped_crossing)[index] = mapped[4094 + index];
	}
	if (mapped_crossing - image != loaded_crossing - library)
		return 8;
	if (!UnmapViewOfFile(view) || !CloseHandle(section) || !CloseHandle(file))
		return 9;
	return 0;
}
#endif
