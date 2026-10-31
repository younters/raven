// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include "fex-module-config.h"
#include <cstdlib>
#include <filesystem>
#include <mach-o/dyld.h>
#include <stdexcept>
#include <vector>

namespace raven {
std::string discover_fex_module_root()
{
	namespace fs = std::filesystem;
	const auto valid = [](const fs::path &root) {
		std::error_code error;
		return fs::is_regular_file(root / "xtajit64.dll", error);
	};
	if (const char *override_root = std::getenv("RAVEN_FEX_MODULE_ROOT"); override_root && *override_root) {
		if (valid(override_root)) return override_root;
		throw std::runtime_error("FEX module override missing xtajit64.dll: " + std::string(override_root));
	}
	std::vector<fs::path> roots;
	if (*RAVEN_FEX_DEFAULT_MODULE_ROOT) roots.emplace_back(RAVEN_FEX_DEFAULT_MODULE_ROOT);
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> executable(size);
	if (!_NSGetExecutablePath(executable.data(), &size)) {
		std::error_code error;
		auto directory = fs::weakly_canonical(executable.data(), error).parent_path();
		if (!error) {
			roots.push_back(directory / "fex-modules");
			roots.push_back(directory / "../raven-components/fex-modules");
		}
	}
	std::string searched;
	for (const auto &root : roots) {
		if (valid(root)) return root.string();
		searched += " " + root.string();
	}
	throw std::runtime_error("FEX modules missing: provision xtajit64.dll with scripts/build-fex-modules.py and set "
		"RAVEN_FEX_MODULE_ROOT, configure it at build time, or place fex-modules beside raven. Searched:" + searched);
}
}
