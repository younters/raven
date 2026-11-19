#pragma once
#include <spawn.h>
#include <string>
#include <vector>

namespace raven_darwin {
void child_launch_metadata(std::vector<std::string> &environment, const std::string &image, posix_spawn_file_actions_t *actions = nullptr);
// Returns once no live process is left in the process group: what Linux's child subreaper lets a parent wait for, the programs its
// child started among them.
void wait_process_group(pid_t group);
}

void raven_macos_startup(int, char **);
