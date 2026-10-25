// The x86 intrinsics the host runtime itself uses, on an Apple silicon host. Rosetta 2 answers a guest's RDTSC with the ARM generic
// timer, so the host reads the same counter to relate the guest's time stamps to its own clocks.
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <x86intrin.h>
#else
#include <stdint.h>

static inline uint64_t __rdtsc(void)
{
	uint64_t counter;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(counter) : : "memory");
	return counter;
}
#endif
