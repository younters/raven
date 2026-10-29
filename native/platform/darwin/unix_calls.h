#pragma once
#include "cpu/native_cpu.h"
#include <map>

namespace raven {

bool darwin_graphics_system_module(std::string_view name, bool x64);
std::optional<ModuleBytes> darwin_graphics_module(std::string_view name, bool x64);

// Wine's PE unixlib discovery ABI, restricted to the explicitly provisioned DXMT driver.
class DarwinUnixCalls {
public:
	DarwinUnixCalls(NativeProcess &process, const ModuleSet &modules);
	bool dispatch(std::string_view name);

private:
	NativeProcess &process;
	const ModuleSet &modules;
	std::map<uint64_t, uint64_t> handles;
};

}
