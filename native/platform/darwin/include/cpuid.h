// An Apple silicon host has no CPUID. The processor a Windows program can use is the one Rosetta 2 presents to the guest, with AVX
// advertised (the controller starts the guest with ROSETTA_ADVERTISE_AVX=1), so host questions about it answer from what that guest
// reads: leaves captured from Rosetta on macOS 27.
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <cpuid.h>
#else

static const unsigned raven_darwin_cpuid_leaves[][6] = {
	{0x0, 0, 0x0000000d, 0x756e6547, 0x6c65746e, 0x49656e69},
	{0x1, 0, 0x000206c0, 0x00010800, 0x7ed8320f, 0x0f8b8b15},
	{0x7, 0, 0x00000000, 0x00000128, 0x00000000, 0x00000000},
	{0xd, 0, 0x00000007, 0x00000340, 0x00000340, 0x00000000},
	{0x80000000, 0, 0x80000008, 0x00000000, 0x00000000, 0x00000000},
	{0x80000001, 0, 0x00000000, 0x00000000, 0x00000021, 0x28100800},
};

static inline void raven_darwin_cpuid(unsigned leaf, unsigned subleaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
	*a = *b = *c = *d = 0;
	for (unsigned i = 0; i < sizeof raven_darwin_cpuid_leaves / sizeof *raven_darwin_cpuid_leaves; i++)
		if (raven_darwin_cpuid_leaves[i][0] == leaf && raven_darwin_cpuid_leaves[i][1] == subleaf) {
			*a = raven_darwin_cpuid_leaves[i][2]; *b = raven_darwin_cpuid_leaves[i][3];
			*c = raven_darwin_cpuid_leaves[i][4]; *d = raven_darwin_cpuid_leaves[i][5];
		}
}

static inline unsigned __get_cpuid_max(unsigned extended, unsigned *signature)
{
	unsigned a, b, c, d;
	raven_darwin_cpuid(extended, 0, &a, &b, &c, &d);
	if (signature)
		*signature = b;
	return a;
}

static inline int __get_cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
	if (__get_cpuid_max(leaf & 0x80000000U, 0) < leaf)
		return 0;
	raven_darwin_cpuid(leaf, 0, a, b, c, d);
	return 1;
}

static inline int __get_cpuid_count(unsigned leaf, unsigned subleaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
	if (__get_cpuid_max(leaf & 0x80000000U, 0) < leaf)
		return 0;
	raven_darwin_cpuid(leaf, subleaf, a, b, c, d);
	return 1;
}

#define __cpuid(leaf, a, b, c, d) raven_darwin_cpuid((leaf), 0, &(a), &(b), &(c), &(d))
#define __cpuid_count(leaf, subleaf, a, b, c, d) raven_darwin_cpuid((leaf), (subleaf), &(a), &(b), &(c), &(d))

#endif
