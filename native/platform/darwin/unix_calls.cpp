#include "unix_calls.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace raven {

namespace {
std::filesystem::path dxmt_root()
{
	if (const char *root = std::getenv("RAVEN_DXMT_ROOT"))
		return root;
#ifdef RAVEN_DXMT_ROOT
	return RAVEN_DXMT_ROOT;
#else
	return {};
#endif
}
}

std::optional<ModuleBytes> darwin_graphics_module(std::string_view name, bool x64)
{
	const auto root = dxmt_root();
	if (!x64 || root.empty() || (name != "d3d11.dll" && name != "dxgi.dll" && name != "d3d10core.dll" && name != "winemetal.dll"))
		return std::nullopt;
	const auto path = root / "pe64" / name;
	if (auto bytes = ModuleBytes::map(path))
		return bytes;
	throw std::runtime_error("configured DXMT module is missing: " + path.string());
}

DarwinUnixCalls::DarwinUnixCalls(NativeProcess &process, const ModuleSet &modules) : process(process), modules(modules)
{
	const auto nt = modules.all().find("ntdll.dll");
	if (nt == modules.all().end() || !nt->second->runtime_api)
		return;
	auto &module = *nt->second;
	const auto symbol = module.export_names.find("__wine_unix_call_dispatcher");
	if (symbol == module.export_names.end())
		return;
	const uint64_t gate = process.unix_call_gate();
	// The provider's image is shared with the agent. This export is data, read through GetProcAddress by Wine's CRT startup.
	std::memcpy(module.loaded.data().data() + module.exports.at(symbol->second).address, &gate, sizeof gate);
}

bool DarwinUnixCalls::dispatch(std::string_view name)
{
	if (name != "NtQueryVirtualMemory" && name != "ZwQueryVirtualMemory")
		return false;
	const auto args = process.api_arguments(6);
	if (args[2] != 1000 && args[2] != 1001)
		return false;
	uint32_t status = 0xc0000135U; // STATUS_DLL_NOT_FOUND
	if (args[0] != UINT64_MAX)
		status = 0xc0000008U;
	else if (args[2] == 1001)
		status = 0xc00000bbU; // WoW64 is not implemented.
	else if (args[4] < 8)
		status = 0xc0000004U;
	else {
		const auto found = modules.all().find("winemetal.dll");
		const auto root = dxmt_root();
		if (found != modules.all().end() && found->second->loaded.base() == args[1] && !root.empty()) {
			try {
				process.validate_write_memory(args[3], 8);
				if (args[5]) process.validate_write_memory(args[5], 8);
				auto handle = handles.find(args[1]);
				if (handle == handles.end()) {
					// v0.80's table has 132 entries, including the reserved NULL at index 83.
					const auto path = root / "unix64" / "winemetal.so";
					handle = handles.emplace(args[1], process.load_unix_library(path.string(), 132)).first;
				}
				process.write_memory(args[3], {reinterpret_cast<const uint8_t *>(&handle->second), 8});
				if (args[5]) {
					const uint64_t size = 8;
					process.write_memory(args[5], {reinterpret_cast<const uint8_t *>(&size), 8});
				}
				status = 0;
			} catch (const std::runtime_error &error) {
				std::cerr << "raven: DXMT unixlib discovery: " << error.what() << '\n';
				status = 0xc0000001U;
			}
		}
	}
	process.return_api(status);
	return true;
}

}
