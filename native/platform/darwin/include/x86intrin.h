// The x86 intrinsics the host runtime itself uses, on an Apple silicon host. Rosetta 2 answers a guest's RDTSC with the ARM generic
// timer in 1 GHz units, so the host reads the same counter when calibrating guest time stamps.
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <x86intrin.h>
#else
#include <stdint.h>

static inline uint64_t __rdtsc(void)
{
	uint64_t counter;
	// Apple's assembler treats ';' as a comment: the instructions must occupy separate lines.
	__asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(counter) : : "memory");
	return counter;
}
#endif
