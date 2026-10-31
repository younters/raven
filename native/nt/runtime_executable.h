// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>

namespace raven {
// Always start children through the modern launcher. Their execution option selects
// the companion when needed; keep engine inheritance independent of path selection.
inline std::filesystem::path child_runtime_executable(std::filesystem::path self)
{
#ifdef __APPLE__
	if (self.filename() == "raven-fex-host")
		return self.parent_path() / "raven";
#endif
	return self;
}
}
