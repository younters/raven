#pragma once

#include <spawn.h>
#include <string>
#include <vector>

namespace raven {

// Only PE32 guests benefit enough to pay for disabling Rosetta's AOT cache.
int spawn_x87_guest(pid_t *pid, const std::string &bootstrap, const std::string &service, bool x86,
	const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes, std::vector<std::string> environment);

}
