// SPDX-License-Identifier: MIT
// Addresses the guest gave up can come back as host memory; a Metal buffer landed on one and every upload to it was refused.
#include "fex/unix_ranges.h"
#include <cstdio>
#include <sys/mman.h>

static int fail(const char *message)
{
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

int main()
{
	const size_t size = 3 * 16384;
	auto *pages = static_cast<char *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
	if (pages == MAP_FAILED) return fail("cannot map test pages");
	const auto base = reinterpret_cast<uint64_t>(pages);
	UnixRanges ranges;
	// A logical protection is enforced even where the host page is writable: a guest page is smaller than a host page.
	ranges.set(base, size, PROT_NONE);
	if (ranges.check(base, 16, 0)) return fail("a recorded no-access range was readable");
	ranges.set(base + 16384, 16384, PROT_READ);
	if (!ranges.check(base + 16384, 16, 0) || ranges.check(base + 16384, 16, 1)) return fail("a read-only range was wrong");
	// Forgetting part of a range leaves the rest, and the real mapping decides what was forgotten.
	ranges.forget(base + 16384, 16384);
	if (!ranges.check(base + 16384, 16, 1)) return fail("forgotten memory that is writable was refused");
	if (ranges.check(base, 16, 0) || ranges.check(base + 2 * 16384, 16, 0)) return fail("forgetting reached past its range");
	// What is forgotten and not mapped at all stays refused.
	ranges.forget(base, size);
	if (!ranges.check(base, size, 1)) return fail("forgotten mapped memory was refused");
	munmap(pages, size);
	if (ranges.check(base, 16, 0)) return fail("unmapped memory was accepted");
	std::puts("PASS: unix ranges follow memory the guest gave up");
	return 0;
}
