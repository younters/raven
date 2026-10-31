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
#ifdef RAVEN_FEX_PROCESS
	// Interim M5 policy: x86 children use Rosetta until M4 WoW64 lands.
	if (is_64 && raven::fex_execution_selected()) return "--execution=fex";
#endif
	return "--execution=native";
}
void child_launch_metadata(std::vector<std::string> &environment, const std::string &image, posix_spawn_file_actions_t *actions = nullptr);
}

void raven_macos_startup(int, char **);
