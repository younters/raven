#pragma once
#include <spawn.h>
#include <string>
#include <vector>

#ifdef RAVEN_FEX_PROCESS
namespace raven { bool fex_execution_selected(); }
#endif
namespace raven_darwin {
inline const char *child_execution_argument(bool is_64 = true)
{
	(void)is_64;
#ifdef RAVEN_FEX_PROCESS
	// Both guest widths inherit FEX; raven redirects through its companion host.
	if (raven::fex_execution_selected()) return "--execution=fex";
#endif
	return "--execution=native";
}
void child_launch_metadata(std::vector<std::string> &environment, const std::string &image, posix_spawn_file_actions_t *actions = nullptr);
}

void raven_macos_startup(int, char **);
