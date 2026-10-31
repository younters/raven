// SPDX-License-Identifier: MIT
#include "fex_process.h"
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
#ifndef RAVEN_FEX_LEGACY_HOST
    // Match the CLI's execution option position; do not interpret guest arguments.
    if (argc < 3 || (std::strcmp(argv[1], "--run-process") && std::strcmp(argv[1], "--run-entry")) ||
        std::strcmp(argv[2], "--execution=fex")) return;
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> executable(size);
    if (!_NSGetExecutablePath(executable.data(), &size)) {
        const auto host = std::filesystem::canonical(executable.data()).parent_path() / "raven-fex-host";
        execv(host.c_str(), argv);
        std::fprintf(stderr, "raven: cannot launch FEX companion %s: %s\n", host.c_str(), std::strerror(errno));
    }
    std::exit(1);
#else
    (void)argc;
    (void)argv;
#endif
}
}
