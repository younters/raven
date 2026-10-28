#pragma once
#include <string>
#include <vector>
#include <spawn.h>
namespace raven_darwin {
void child_launch_metadata(std::vector<std::string> &, const std::string &image, posix_spawn_file_actions_t * = nullptr);
}

void raven_macos_startup(int,char **);
