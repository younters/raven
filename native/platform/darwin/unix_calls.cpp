#include "unix_calls.h"
#include "mullion/backend.h"
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace raven {

namespace {
// Keep these counts in step with the pinned graphics components.
constexpr uint32_t mtld3d_table_count = 22; // mtld3d v0.15.1
constexpr uint32_t dxmt_release_table_count = 132; // DXMT v0.80 release
constexpr uint32_t dxmt_source_table_count = 146; // DXMT at 589adb780354
constexpr uint32_t mullion_table_count = 197; // Mullion at 46e6f0508c4d

std::filesystem::path dxmt_root()
{
	if (mullion_selected()) return mullion_root();
	if (const char *root = std::getenv("RAVEN_DXMT_ROOT"))
		return root;
#ifdef RAVEN_DXMT_ROOT
	return RAVEN_DXMT_ROOT;
#else
	return {};
#endif
}
}

bool darwin_graphics_system_module(std::string_view name, bool x64)
{
    if (!x64 || dxmt_root().empty()) return false;
    if (mullion_selected() && (name == "d3d12.dll" || name == "d3d12core.dll")) return true;
    if (name == "d3d11.dll" || name == "dxgi.dll" || name == "d3d10core.dll" || name == "winemetal.dll" || name == "d3d10.dll" ||
        name == "d3d10_1.dll")
        return true;
    // The runtime's dcomp.dll forwards device creation to the DXMT composition device.
    return name == "dcomp-metal.dll" && std::filesystem::is_regular_file(dxmt_root() / "pe64/dcomp-metal.dll");
}

std::optional<ModuleBytes> darwin_graphics_module(std::string_view name, bool x64)
{
    if (!darwin_graphics_system_module(name, x64)) return std::nullopt;
    const auto root = dxmt_root();
	const auto path = root / "pe64" / name;
	if (auto bytes = ModuleBytes::map(path)) {
		if (mullion_selected()) std::cerr << "raven: Mullion module loaded: " << path.string() << '\n';
		return bytes;
	}
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
	std::memcpy(module.loaded.data().data() + module.exports.at(symbol->second).address, &gate, process.mode() == NativeMode::x86 ? 4 : 8);
}

bool DarwinUnixCalls::dispatch(std::string_view name)
{
	if (name == "__wine_dbg_output") {
		uint64_t address = process.api_arguments(1)[0];
		std::string text;
		while (text.size() < 65536) {
			const size_t count = std::min<size_t>(256, 4096 - address % 4096);
			const auto bytes = process.read_memory(address, count);
			const auto end = std::find(bytes.begin(), bytes.end(), 0);
			text.append(bytes.begin(), end);
			if (end != bytes.end()) break;
			address += count;
		}
		std::cerr << text;
		process.return_api(text.size());
		return true;
	}
	if (name != "NtQueryVirtualMemory" && name != "ZwQueryVirtualMemory")
		return false;
	const auto args = process.api_arguments(6);
	if (args[2] != 1000 && args[2] != 1001)
		return false;
	uint32_t status = 0xc0000135U; // STATUS_DLL_NOT_FOUND
	if (args[0] != UINT64_MAX && !(process.mode() == NativeMode::x86 && args[0] == UINT32_MAX))
		status = 0xc0000008U;
	else if (args[2] == 1001 && process.mode() != NativeMode::x86)
		status = 0xc00000bbU; // WoW64 is not implemented.
	else if (args[4] < 8)
		status = 0xc0000004U;
	else {
		auto found = modules.all().find("mtld3d.dll");
		const bool mtld3d = found != modules.all().end() && found->second->loaded.base() == args[1];
		if (std::getenv("RAVEN_TRACE_UNIX")) {
			std::cerr << "raven: unixlib discovery class " << args[2] << " for 0x" << std::hex << args[1] << ':';
			for (const auto &[module_name, module] : modules.all())
				if (module->loaded.base() == args[1])
					std::cerr << ' ' << module_name;
			std::cerr << std::dec << (mtld3d ? " (mtld3d)" : "") << '\n';
		}
		if (!mtld3d) found = modules.all().find("winemetal.dll");
		const char *mtld_root = std::getenv("RAVEN_MTLD3D_ARM64_ROOT");
		const auto root = mtld3d ? std::filesystem::path(mtld_root ? mtld_root : "") : dxmt_root();
		if (found != modules.all().end() && found->second->loaded.base() == args[1] && !root.empty()) {
			try {
				process.validate_write_memory(args[3], 8);
				if (args[5]) process.validate_write_memory(args[5], process.mode() == NativeMode::x86 ? 4 : 8);
				auto handle = handles.find(args[1]);
				if (handle == handles.end()) {
					const auto path = mtld3d ? root / "mtld3d.so" : root / "unix64" / "winemetal.so";
                    if (!std::filesystem::is_regular_file(path))
                        throw std::runtime_error("overlay missing at " + path.string());
					// Older DXMT release overlays have no count file. If present, the metadata must match a pinned table.
					uint32_t count = mtld3d ? mtld3d_table_count : dxmt_release_table_count;
					const bool mullion = !mtld3d && mullion_selected();
					const auto count_path = mtld3d ? root / "unix-call-count" : root / "unix64" / "unix-call-count";
					if (mullion && !std::filesystem::is_regular_file(count_path))
						throw std::runtime_error("Mullion unix-call-count metadata missing");
					if (std::filesystem::exists(count_path)) {
						std::ifstream input(count_path);
						if (!(input >> count) || (mtld3d ? count != mtld3d_table_count : mullion ? count != mullion_table_count :
							count != dxmt_release_table_count && count != dxmt_source_table_count))
							throw std::runtime_error("unsupported DXMT unix table size");
					}
					handle = handles.emplace(args[1], process.load_unix_library(path.string(), count)).first;
				}
				process.write_memory(args[3], {reinterpret_cast<const uint8_t *>(&handle->second), 8});
				if (args[5]) {
					const uint64_t size = 8;
					process.write_memory(args[5], {reinterpret_cast<const uint8_t *>(&size), process.mode() == NativeMode::x86 ? size_t(4) : size_t(8)});
				}
				status = 0;
			} catch (const std::runtime_error &error) {
				std::cerr << "raven: graphics unixlib discovery: " << error.what() << '\n';
				status = 0xc0000001U;
			}
		}
	}
	process.return_api(status);
	return true;
}

}
