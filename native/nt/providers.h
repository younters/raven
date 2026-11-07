#pragma once

#include "environment.h"
#include "desktop/presentation.h"
#include <functional>
#include <optional>

namespace raven {

// Original name-only PE64 modules for implemented environment and loader handlers.
std::vector<uint8_t> environment_provider(std::string_view name, uint64_t base);
std::vector<uint8_t> runtime_provider(std::string_view name, uint64_t base,
	std::span<const std::string_view> exports, bool x86 = false,
	std::span<const std::string_view> data_exports = {},
	std::span<const std::pair<std::string_view, uint16_t>> ordinal_exports = {},
	// Gives hot kernel32 services guest-side fast paths; their slow-path trap sits at a fixed offset inside the stub.
	bool fast_paths = false);
// Where the breakpoint of a 64-bit provider export sits inside its stub: past the guest-side path of a fast or served stub, at its start
// otherwise.
size_t fast_stub_trap_offset(std::string_view module, std::string_view name);
// Whether providers carry guest-side paths; RAVEN_NO_GUEST_PATHS serves every call in the runtime, to compare behaviour.
bool guest_paths();
// Where the breakpoint of a 32-bit provider export sits: past the guest-side path of a fast stub, at its start otherwise.
uint64_t x86_api_trap_address(const PEModule &provider, uint32_t address);
// The name the runtime serves a 32-bit fast stub's own trap under, where the stub calls it for a slow path rather than handing over the call.
std::string_view x86_fast_stub_slow_name(std::string_view name);
// The breakpoint a 64-bit provider export ends in when the runtime serves it: behind a guest-side prelude when the export has one.
uint64_t api_trap_address(const PEModule &provider, std::string_view module, std::string_view name, uint32_t address);
// Device information sets belong to the runtime; the installer component handles INF files and queues.
bool host_device_export(std::string_view library, std::string_view name);
// RAVEN_API_INVENTORY names a TSV file recording the actual host registries, guest exports and provider forwarders of this graph.
void write_native_api_inventory(const ModuleSet &modules, std::initializer_list<const class ApiRegistry *> registries);
// Runs a PE32 process in host compatibility mode with the shared services and 32-bit guest components.
// A source of libraries shipped beside the program lets a program import them statically: they are loaded with it, and their entry points run
// before the program's.
NativeResult run_native_process_x86(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout, std::optional<NativeLaunch> launch = {},
	DesktopPresentation presentation = {},
	std::function<std::optional<ModuleBytes>(std::string_view)> source = {});
NativeResult run_native_process(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout,
	std::function<std::optional<ModuleBytes>(std::string_view)> source = {},
	std::function<void(std::span<const uint8_t>)> debug = {}, DesktopPresentation presentation = {},
	std::optional<NativeLaunch> launch = {});

}
