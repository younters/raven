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
extern "C" char raven_pe_reserve_end[];
#endif
static void reserve_main_image(int argc, char **argv)
{
#ifdef RAVEN_FEX_COMPANION_HOST
	// Keep the Mach-O segment reserved: dyld still describes it as part of the
	// host image. Reusing its unused pages for host dylibs/JIT confuses ownership.
	const auto pool = reinterpret_cast<uint64_t>(raven_pe_reserve);
    const auto pool_end = reinterpret_cast<uint64_t>(raven_pe_reserve_end);
    if (pool_end <= pool || pool > 0x140000000ULL || 0x140000000ULL >= pool_end) {
        std::fprintf(stderr, "raven: FEX image pool does not cover 0x140000000\n");
        std::exit(1);
    }
#ifdef RAVEN_FEX_PLACEMENT_TEST_HOOKS
    if (!std::getenv("RAVEN_FEX_DISABLE_IMAGE_POOL"))
#endif
        fex_register_image_reservation(pool, pool_end - pool);
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
	if (!fex_reserve_image_address(base, size)) {
        std::fprintf(stderr, "raven: FEX early image reservation failed at %llx\n", (unsigned long long)base);
        std::exit(1);
    }
    if (fex_image_pool_claimed(base))
        std::fprintf(stderr, "FEX main image claimed inside image_pool\n");
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
#ifdef RAVEN_FEX_PLACEMENT_TEST_HOOKS
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
#endif
	// A legitimate companion consumes the marker so its children may select FEX again.
	::unsetenv("RAVEN_FEX_REEXEC");
	(void)argc;
	(void)argv;
#endif
}
}
