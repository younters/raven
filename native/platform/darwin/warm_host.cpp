// The warm hosts of nt/warm_host.h fork a host after it loaded the system's libraries, and macOS does not support a forked child using
// the frameworks its parent loaded (AppKit, Metal, CoreAudio). Every start on macOS is an ordinary one: the launcher never asks for a
// warm host (warm_client.cpp), and these are the ordinary start's answers.
#include "nt/warm_host.h"

namespace raven {

void start_warm_host(int, char **) {}
void warm_point(const std::function<void()> &) {}
bool warm_ready() { return false; }
bool warm_waiting() { return false; }
void warm_when_ready(std::function<void()> work) { work(); }
void warm_defer(std::function<void()> work) { work(); }
void warm_settle() {}
void warm_watch(const std::filesystem::path &) {}

}
