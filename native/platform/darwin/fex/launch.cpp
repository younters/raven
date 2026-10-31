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

namespace raven {
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
	// A legitimate companion consumes the marker so its children may select FEX again.
	::unsetenv("RAVEN_FEX_REEXEC");
	(void)argc;
	(void)argv;
#endif
}
}
