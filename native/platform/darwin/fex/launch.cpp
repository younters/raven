// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <vector>
#include <fcntl.h>
#include <cstdint>
#include <dlfcn.h>
#include <sys/mman.h>

namespace raven {
// Read only the fixed PE header here: this runs before Cocoa identity setup,
// prefix initialization, image buffering, or any optional host dlopen. The normal
// PE loader still validates the complete file and owns the reservation thereafter.
#ifdef RAVEN_FEX_COMPANION_HOST
extern "C" char raven_pe_reserve[];
#endif
static void reserve_main_image(int argc, char **argv)
{
#ifdef RAVEN_FEX_COMPANION_HOST
	// The Mach-O zero-fill segment is already owned before dyld runs initializers.
	// ASLR slides it with the host; use the symbol, never an assumed slide.
	const auto start = reinterpret_cast<uint64_t>(raven_pe_reserve);
	const auto end = start + 0x100000000ULL;
	struct Release {
		uint64_t start, end, keep = 0, size = 0;
		~Release() {
			if (!size) munmap(reinterpret_cast<void *>(start), end - start);
			else {
				if (keep > start) munmap(reinterpret_cast<void *>(start), keep - start);
				if (keep + size < end) munmap(reinterpret_cast<void *>(keep + size), end - keep - size);
			}
		}
	} release{start, end};
#endif
	if (argc < 4 || std::strcmp(argv[2], "--execution=fex")) return;
	int index = 3;
	while (index < argc && !std::strncmp(argv[index], "--", 2)) ++index;
	if (index == argc) return;
	const int fd = open(argv[index], O_RDONLY | O_CLOEXEC);
	if (fd < 0) return;
	uint8_t dos[64]{}, pe[88]{};
	uint32_t offset = 0;
	bool valid = pread(fd, dos, sizeof(dos), 0) == sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z';
	std::memcpy(&offset, dos + 60, sizeof(offset));
	valid = valid && pread(fd, pe, sizeof(pe), offset) == sizeof(pe);
	close(fd);
	if (!valid || std::memcmp(pe, "PE\0\0", 4) || pe[24] != 0x0b || pe[25] != 2) return;
	uint64_t base = 0;
	uint32_t size = 0;
	std::memcpy(&base, pe + 48, sizeof(base));
	std::memcpy(&size, pe + 80, sizeof(size));
	if (base < 0x100000000ULL || base % 65536 || !size || base > UINT64_MAX - size - 16383) return;
#ifdef RAVEN_FEX_COMPANION_HOST
	const auto length = (uint64_t(size) + 16383) & ~uint64_t(16383);
	if (base >= start && base <= end && length <= end - base) {
		if (mprotect(reinterpret_cast<void *>(base), length, PROT_NONE)) std::exit(1);
		fex_adopt_image_reservation(base, length);
		if (std::getenv("RAVEN_FEX_TRACE_MEMORY"))
			std::fprintf(stderr, "FEX startup reservation %llx+%llx from Mach-O %llx+100000000\n",
				static_cast<unsigned long long>(base), static_cast<unsigned long long>(length),
				static_cast<unsigned long long>(start));
		release.keep = base;
		release.size = length;
		return;
	}
#endif
	(void)fex_reserve_image_address(base, size);
}

void launch_fex_host(int argc, char **argv)
{
#ifndef RAVEN_FEX_COMPANION_HOST
	// Match the CLI's execution option position; do not interpret guest arguments.
	if (argc < 3 || (std::strcmp(argv[1], "--run-process") && std::strcmp(argv[1], "--run-entry") &&
		std::strcmp(argv[1], "--run-modules")) || std::strcmp(argv[2], "--execution=fex")) return;
	if (std::getenv("RAVEN_FEX_REEXEC")) {
		std::fprintf(stderr, "raven: FEX companion re-exec loop: raven-fex-host must be the companion binary\n");
		std::exit(1);
	}
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> executable(size);
	if (!size || _NSGetExecutablePath(executable.data(), &size)) {
		std::fprintf(stderr, "raven: cannot locate executable for FEX companion\n");
		std::exit(1);
	}
	std::error_code error;
	const auto resolved = std::filesystem::canonical(executable.data(), error);
	if (error) {
		std::fprintf(stderr, "raven: cannot resolve executable for FEX companion: %s\n", error.message().c_str());
		std::exit(1);
	}
	const auto host = resolved.parent_path() / "raven-fex-host";
	if (::setenv("RAVEN_FEX_REEXEC", "1", 1)) {
		std::fprintf(stderr, "raven: cannot set FEX companion re-exec guard: %s\n", std::strerror(errno));
		std::exit(1);
	}
	execv(host.c_str(), argv);
	std::fprintf(stderr, "raven: cannot launch FEX companion %s: %s\n", host.c_str(), std::strerror(errno));
	std::exit(1);
#else
	reserve_main_image(argc, argv);
	// Test-only startup pressure, deliberately after reservation and before the
	// normal loader. Keep the allocations and library alive through image mapping.
	if (const char *library = std::getenv("RAVEN_FEX_PLACEMENT_PRESSURE")) {
		for (size_t n = 0; n < 64; ++n) {
			auto *p = static_cast<volatile char *>(std::malloc(1024 * 1024));
			if (!p) std::exit(1);
			for (size_t page = 0; page < 1024 * 1024; page += 4096) p[page] = 1;
		}
		if (!dlopen(library, RTLD_NOW | RTLD_LOCAL)) {
			std::fprintf(stderr, "FEX placement pressure: %s\n", dlerror());
			std::exit(1);
		}
		std::fprintf(stderr, "FEX placement pressure applied\n");
	}
	// A legitimate companion consumes the marker so its children may select FEX again.
	::unsetenv("RAVEN_FEX_REEXEC");
	(void)argc;
	(void)argv;
#endif
}
}
