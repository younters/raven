// SPDX-License-Identifier: MIT
#include "guest_window.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <initializer_list>
#include <utility>

int main()
{
	try {
		raven::FexGuestWindow window;
		if (!window.base() || window.base() % window.size || window.base() < window.size)
			throw std::runtime_error("window is not high and 4 GiB aligned");
		for (const uint64_t guest : {0x10000ULL, 0x400000ULL, 0x7ffe0000ULL, 0xffffc000ULL}) {
			auto *host = static_cast<uint32_t *>(window.host(guest, 16384));
			if (mprotect(host, 16384, PROT_READ | PROT_WRITE))
				throw std::runtime_error("cannot commit guest-window page");
			*host = uint32_t(guest);
			if (*host != guest) throw std::runtime_error("window backing mismatch");
			if (mprotect(host, 16384, PROT_NONE))
				throw std::runtime_error("cannot restore reservation protection");
		}
		for (const auto range : std::initializer_list<std::pair<uint64_t, size_t>>{{0, 1}, {0xffff, 1}, {0xffffffff, 2}, {0x100000000ULL, 0}, {UINT64_MAX, 1}}) {
			bool rejected = false;
			try { (void)window.host(range.first, range.second); }
			catch (const std::runtime_error &) { rejected = true; }
			if (!rejected) throw std::runtime_error("invalid guest range accepted");
		}
		std::printf("PASS: Tier 0 guest window base=0x%llx size=0x%llx host_page=%ld\n",
			static_cast<unsigned long long>(window.base()), static_cast<unsigned long long>(window.size), (sysconf)(_SC_PAGESIZE));
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
