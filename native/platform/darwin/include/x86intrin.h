// The x86 intrinsics the host runtime itself uses, on an Apple silicon host. Rosetta 2 answers a guest's RDTSC with the ARM generic
// timer in 1 GHz units, so the host reads the same counter when calibrating guest time stamps.
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <x86intrin.h>
#else
#include <stdint.h>
#include <mach/mach_time.h>

static inline uint64_t __rdtsc(void)
{
	uint64_t counter;
	// Apple's assembler treats ';' as a comment: the instructions must occupy separate lines.
	__asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(counter) : : "memory");
	// CNTVCT is in hardware ticks (24 MHz on some Macs), while Rosetta
	// exposes this same clock in nanoseconds, including its absolute origin.
	mach_timebase_info_data_t scale;
	mach_timebase_info(&scale);
	return (uint64_t)((__uint128_t)counter * scale.numer / scale.denom);
}
#endif
