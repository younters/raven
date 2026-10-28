#pragma once
#include <spawn.h>
#include <string>
#include <vector>

namespace raven_darwin {
void child_launch_metadata(std::vector<std::string> &environment, const std::string &image, posix_spawn_file_actions_t *actions = nullptr);
}

void raven_macos_startup(int, char **);
