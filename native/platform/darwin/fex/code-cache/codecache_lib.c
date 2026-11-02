// SPDX-License-Identifier: MIT
// Built twice with the same name, timestamp and size; only CODECACHE_VARIANT differs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifndef CODECACHE_VARIANT
#define CODECACHE_VARIANT 1
#endif

BOOL WINAPI raven_dll_entry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}

// Many small blocks with data-dependent branches, so a stale translation changes the result.
static uint32_t __attribute__((noinline)) mix(uint32_t h, uint32_t v)
{
	switch (v & 7) {
	case 0: h ^= v * 0x9e3779b1u; break;
	case 1: h += (v << 7) | (v >> 25); break;
	case 2: h = (h << 5) - h + v; break;
	case 3: h ^= h >> 13; h *= 0x85ebca6bu; break;
	case 4: h += v ^ 0xc2b2ae35u; break;
	case 5: h = (h >> 3) | (h << 29); break;
	case 6: h -= v * 3; break;
	default: h ^= (uint32_t)CODECACHE_VARIANT * 0x01000193u; break;
	}
	return h;
}

__declspec(dllexport) uint32_t codecache_compute(uint32_t rounds)
{
	uint32_t h = 0x811c9dc5u, v = 1;
	for (uint32_t i = 0; i < rounds; ++i) {
		v = v * 1103515245u + 12345u;
		h = mix(h, v >> 8);
		if (h & 0x100) h += i;
		else h ^= i << 3;
	}
	return h;
}
