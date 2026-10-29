#include "x87sidecar.h"

#include <crt_externs.h>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace raven {

int spawn_x87_guest(pid_t *pid, const std::string &bootstrap, const std::string &service, bool x86,
	const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes, std::vector<std::string> environment)
{
	std::string sidecar;
	if (x86) {
		if (const char *value = std::getenv("RAVEN_X87_SIDECAR"))
			sidecar = value; // An explicit empty value disables automatic discovery.
		else if (const char *home = std::getenv("HOME"))
			sidecar = std::string(home) + "/Documents/Projects/raven-components/x87sidecar/x87sidecar";
	}
	const bool requested = !sidecar.empty();
	if (requested && access(sidecar.c_str(), X_OK) != 0) {
		// Missing automatic provisioning is the ordinary, silent path.
		if (std::getenv("RAVEN_X87_SIDECAR"))
			std::fprintf(stderr, "raven: RUNNING WITHOUT X87 ACCELERATION: sidecar unavailable: %s\n", sidecar.c_str());
		sidecar.clear();
	}
	const auto pointers = [](std::vector<std::string> &values) {
		std::vector<char *> env;
		for (auto &value : values) env.push_back(value.data());
		env.push_back(nullptr);
		return env;
	};
	char *bare[] = {const_cast<char *>(bootstrap.c_str()), const_cast<char *>(service.c_str()), nullptr};
	if (!sidecar.empty()) {
		// The bootstrap otherwise receives a deliberately minimal native environment. Forward sidecar configuration, including
		// its fallback test switches, to the sidecar launch only, so a direct fallback launch keeps the ordinary environment.
		auto wrapped_environment = environment;
		for (char **entry = *_NSGetEnviron(); *entry; ++entry)
			if (std::string_view(*entry).starts_with("X87_") && !std::string_view(*entry).starts_with("X87_SIDECAR_BOOTSTRAP="))
				wrapped_environment.emplace_back(*entry);
		if (const char *aot = std::getenv("ROSETTA_DISABLE_AOT"))
			wrapped_environment.emplace_back(std::string("ROSETTA_DISABLE_AOT=") + aot);
		if (std::getenv("RAVEN_TRACE_API"))
			wrapped_environment.emplace_back("RAVEN_TRACE_API=1");
		auto env = pointers(wrapped_environment);
		char *wrapped[] = {sidecar.data(), const_cast<char *>("--cooperative"), bare[0], bare[1], nullptr};
		// v1.9.0 execs the bootstrap in its original pid; the detached sidecar
		// watches that pid. Raven retains its existing task-port identity checks.
		const int result = posix_spawn(pid, sidecar.c_str(), actions, attributes, wrapped, env.data());
		if (!result) return 0;
		std::fprintf(stderr, "raven: RUNNING WITHOUT X87 ACCELERATION: sidecar spawn error %d\n", result);
	}
	auto env = pointers(environment);
	return posix_spawn(pid, bootstrap.c_str(), actions, attributes, bare, env.data());
}

}
