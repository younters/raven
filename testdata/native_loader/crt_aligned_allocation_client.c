#include <stddef.h>
#include <stdint.h>
#ifndef RAVEN_ALIGNED_HOST_TEST
#include <windows.h>
#endif

/* Public contracts: Microsoft Learn aligned-malloc and aligned-offset-malloc.
 * Raven returns the documented continuation errors directly; handler dispatch
 * and the exact _HEAP_MAXREQ limit are outside this fixture's scope. */
static int aligned_allocation_contract(void *(*allocate)(size_t, size_t),
	void *(*offset_allocate)(size_t, size_t, size_t), void (*release)(void *), int *(*error)(void))
{
	const size_t maximum = (size_t)-1;
	const size_t alignments[] = {1, 2, 4, 8, 16, 64, 4096};
	for (unsigned i = 0; i < sizeof alignments / sizeof alignments[0]; ++i) {
		size_t alignment = alignments[i];
		unsigned char *block = allocate(257, alignment);
		if (!block || ((uintptr_t)block & (alignment - 1))) return 1;
		for (size_t j = 0; j < 257; ++j) block[j] = (unsigned char)j;
		for (size_t j = 0; j < 257; ++j) if (block[j] != (unsigned char)j) return 2;
		release(block);
		const size_t offsets[] = {0, 1, 7, 63, 64, 129, 256};
		for (unsigned k = 0; k < sizeof offsets / sizeof offsets[0]; ++k) {
			block = offset_allocate(257, alignment, offsets[k]);
			if (!block || (((uintptr_t)block + offsets[k]) & (alignment - 1))) return 3;
			for (size_t j = 0; j < 257; ++j) block[j] = (unsigned char)(j + k);
			for (size_t j = 0; j < 257; ++j) if (block[j] != (unsigned char)(j + k)) return 4;
			release(block);
		}
	}
	release(NULL);
	*error() = 0;
	if (allocate(1, 0) || *error() != 22) return 10;
	*error() = 0;
	if (allocate(1, 3) || *error() != 22) return 11;
	*error() = 0;
	if (allocate(0, 16) || *error() != 22) return 12;
	*error() = 0;
	if (offset_allocate(8, 0, 0) || *error() != 22) return 13;
	*error() = 0;
	if (offset_allocate(8, 3, 0) || *error() != 22) return 14;
	*error() = 0;
	if (offset_allocate(8, 16, 8) || *error() != 22) return 15;
	*error() = 0;
	if (offset_allocate(0, 16, 1) || *error() != 22) return 16;
	/* Every total below would wrap to a tiny allocation before the fix.
	 * Never dereference an unexpected non-NULL result for these requests. */
	const size_t boundary[] = {maximum, maximum - 63, maximum - 64,
		maximum - 64 - sizeof(void *) + 1};
	for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; ++i) {
		*error() = 0;
		if (allocate(boundary[i], 64) || *error() != 12) return 20 + i;
		*error() = 0;
		if (offset_allocate(boundary[i], 64, 0) || *error() != 12) return 24 + i;
	}
	*error() = 0;
	if (offset_allocate(maximum - 64 - 2 * sizeof(void *) + 1, 64, 129) || *error() != 12) return 28;
	/* The largest power-of-two alignment also exercises subtraction bounds. */
	*error() = 0;
	if (allocate(maximum / 2 + 1, maximum / 2 + 1) || *error() != 12) return 29;
	*error() = 0;
	if (offset_allocate(maximum / 2 + 1, maximum / 2 + 1, 1) || *error() != 12) return 30;
	return 0;
}

#ifndef RAVEN_ALIGNED_HOST_TEST
int raven_entry(void)
{
	HMODULE crt = LoadLibraryA("ucrtbase.dll");
	if (!crt) return 40;
	void *(*allocate)(size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_malloc");
	void *(*offset_allocate)(size_t, size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_offset_malloc");
	void (*release)(void *) = (void *)GetProcAddress(crt, "_aligned_free");
	int *(*error)(void) = (void *)GetProcAddress(crt, "_errno");
	if (!allocate || !offset_allocate || !release || !error) return 41;
	return aligned_allocation_contract(allocate, offset_allocate, release, error);
}
#endif
