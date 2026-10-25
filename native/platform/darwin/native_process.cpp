// macOS implementation of NativeProcess: the guest is an x86_64 Mach-O agent (guest/agent.c) that runs under Rosetta 2.
//
// Linux drives a ptraced ELF child from outside (cpu/native_cpu.cpp). Rosetta exposes neither ptrace nor the x86 register state of a
// translated thread to another task, so on macOS the agent catches its own traps and faults with signal handlers and exchanges register
// contexts with the host through shared memory; the host maps, protects and reads guest memory through the agent's Mach task port.
//
// Phase 1 of the port: the interface links, and every operation reports that the macOS controller is not there yet.

#include "cpu/native_cpu.h"

#include <algorithm>
#include <stdexcept>

namespace raven {

struct NativeProcess::State {};

[[noreturn]] static void unavailable(const char *operation)
{
	throw std::runtime_error(std::string("the macOS guest controller does not implement ") + operation + " yet");
}

NativeProcess::NativeProcess(std::span<LoadedImage *const>, const std::string &, std::chrono::milliseconds, NativeMode, uint64_t)
{
	unavailable("process creation");
}

NativeProcess::~NativeProcess() = default;

NativeResult NativeProcess::call(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout) { unavailable("call"); }
NativeResult NativeProcess::callback(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout) { unavailable("callback"); }
NativeResult NativeProcess::resume(std::chrono::milliseconds timeout) { unavailable("resume"); }
NativeContext NativeProcess::context() const { unavailable("context"); }
void NativeProcess::set_context(const NativeContext &context) { unavailable("set_context"); }
std::vector<uint8_t> NativeProcess::read_memory(uint64_t address, size_t size) const { unavailable("read_memory"); }
void NativeProcess::write_memory(uint64_t address, std::span<const uint8_t> bytes) { unavailable("write_memory"); }
void NativeProcess::validate_write_memory(uint64_t address,size_t size) const { unavailable("validate_write_memory"); }
uint64_t NativeProcess::compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size) { unavailable("compare_exchange"); }
uint64_t NativeProcess::map_memory(size_t size, int protection, uint64_t address) { unavailable("map_memory"); }
void NativeProcess::protect_memory(uint64_t address, size_t size, int protection) { unavailable("protect_memory"); }
bool NativeProcess::has_protection(uint64_t address, size_t size, int protection) const { unavailable("has_protection"); }
void NativeProcess::protect_image(uint64_t address, size_t size, int protection) { unavailable("protect_image"); }
void NativeProcess::unmap_memory(uint64_t address) { unavailable("unmap_memory"); }
uint64_t NativeProcess::map_shared(int descriptor, size_t size, uint64_t offset) { unavailable("map_shared"); }
uint64_t NativeProcess::map_descriptor(int descriptor, size_t size, uint64_t offset) { unavailable("map_descriptor"); }
bool NativeProcess::memory_available(uint64_t address, size_t size) const { unavailable("memory_available"); }
void NativeProcess::discard_memory(uint64_t address, size_t size) { unavailable("discard_memory"); }
void NativeProcess::map_image(LoadedImage &image) { unavailable("map_image"); }
void NativeProcess::unmap_image(uint64_t address) { unavailable("unmap_image"); }
std::pair<uint64_t, uint64_t> NativeProcess::stack_bounds() const { unavailable("stack_bounds"); }
bool NativeProcess::active() const { unavailable("active"); }
void NativeProcess::abandon_call() { unavailable("abandon_call"); }
void NativeProcess::end_call() { unavailable("end_call"); }
void NativeProcess::add_api_trap(uint64_t address, uint32_t callee_cleanup) { unavailable("add_api_trap"); }
NativeMode NativeProcess::mode() const { unavailable("mode"); }
void NativeProcess::set_teb(uint64_t base) { unavailable("set_teb"); }
void NativeProcess::enter_nt_service(uint64_t instruction, uint32_t number) { unavailable("enter_nt_service"); }
void NativeProcess::complete_debug_print(uint64_t instruction) { unavailable("complete_debug_print"); }
uint64_t NativeProcess::return_gate() const { unavailable("return_gate"); }
size_t NativeProcess::callback_depth() const { unavailable("callback_depth"); }
NativeContext NativeProcess::callback_origin(size_t index) const { unavailable("callback_origin"); }
uint64_t NativeProcess::result_register() const { unavailable("result_register"); }
void NativeProcess::watch_write(uint64_t address) { unavailable("watch_write"); }
void NativeProcess::trace_instructions(size_t count, std::function<void(uint64_t)> sink) { unavailable("trace_instructions"); }
NativeThreadState NativeProcess::save_thread(bool snapshot_registers) const { unavailable("save_thread"); }
void NativeProcess::load_thread(const NativeThreadState &thread) { unavailable("load_thread"); }
NativeThreadState NativeProcess::new_thread(uint64_t stack_top, uint64_t teb) { unavailable("new_thread"); }
bool NativeProcess::parallel() const { unavailable("parallel"); }
bool NativeProcess::stopped() const { unavailable("stopped"); }
int NativeProcess::execution_descriptor() const { unavailable("execution_descriptor"); }
void NativeProcess::drain_execution() { unavailable("drain_execution"); }
void NativeProcess::set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait) { unavailable("set_execution_wait"); }
void NativeProcess::stop_thread(const NativeThreadState &thread) { unavailable("stop_thread"); }
void NativeProcess::pause_thread(NativeThreadState &thread) { unavailable("pause_thread"); }
void NativeProcess::enter_fault_dispatch() { unavailable("enter_fault_dispatch"); }
void NativeProcess::redirect(const NativeContext &context, size_t depth) { unavailable("redirect"); }
std::vector<uint64_t> NativeProcess::api_arguments(size_t count) const { unavailable("api_arguments"); }
void NativeProcess::set_api_argument(size_t index, uint32_t value) { unavailable("set_api_argument"); }
void NativeProcess::set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield) { unavailable("set_time_slicing"); }
void NativeProcess::begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats) { unavailable("begin_translated_call"); }
uint64_t NativeProcess::end_translated_call() { unavailable("end_translated_call"); }
std::array<uint64_t, 4> NativeProcess::api_float_arguments() const { unavailable("api_float_arguments"); }
void NativeProcess::return_api(uint64_t value) { unavailable("return_api"); }
void NativeProcess::set_served_calls(uint64_t *block, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset, std::vector<uint64_t> yield_traps, std::function<void(uint64_t stub)> handler, std::vector<uint64_t> queued_stubs) { unavailable("set_served_calls"); }
uint64_t NativeProcess::serving_teb() const { unavailable("serving_teb"); }
bool NativeProcess::serving_queued() const { unavailable("serving_queued"); }
void NativeProcess::set_queued_gl_elements(bool available) { unavailable("set_queued_gl_elements"); }
void NativeProcess::set_resume_hook(std::function<void()> hook) { unavailable("set_resume_hook"); }
void NativeProcess::set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t rip)> sampler) { unavailable("set_guest_sampler"); }
// Same as the Linux controller's: only NativeProcess is platform-specific.
static NativeResult execute(const PEImage &image, LoadedImage &loaded, std::span<LoadedImage *const> images,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	NativeProcess process(images, bootstrap, timeout);
	return process.call(loaded.base() + image.entry, {}, timeout);
}

NativeResult run_native_entry(const PEImage &image, LoadedImage &loaded,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if (!image.is_64)
		throw std::runtime_error("native PE32 execution is not implemented");
	if ((image.characteristics & 0x2000) != 0 || !image.imports().empty() ||
		image.directories[9].size != 0 || image.directories[13].size != 0 || image.directories[14].size != 0)
		throw std::runtime_error("native entry requires an executable without imports, TLS, delayed imports or CLR initialization");
	LoadedImage *images[] = {&loaded};
	return execute(image, loaded, images, bootstrap, timeout);
}

NativeResult run_native_modules(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if ((entry.image.characteristics & 0x2000) != 0 ||
		!std::any_of(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second.get() == &entry; }))
		throw std::runtime_error("native module entry must be an executable in the supplied graph");
	std::vector<LoadedImage *> images;
	for (const auto &[name, module] : modules.all()) {
		if (!module->image.is_64)
			throw std::runtime_error("native PE32 execution is not implemented");
		if (module->image.directories[9].size || module->image.directories[13].size || module->image.directories[14].size ||
			((module->image.characteristics & 0x2000) != 0 && module->image.entry))
			throw std::runtime_error("native module requires unsupported DLL, TLS, delayed import or CLR initialization: " + name);
		images.push_back(&module->loaded);
	}
	modules.bind_imports();
	return execute(entry.image, entry.loaded, images, bootstrap, timeout);
}

}
