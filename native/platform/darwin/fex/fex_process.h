// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/native_cpu.h"

namespace raven {

// Experimental stopped-context engine. NativeProcess integration is a separate gate.
// One module instance per host process, on its constructing thread.
class FexProcess {
public:
	FexProcess(std::span<LoadedImage *const> images, const std::string &module_root);
	~FexProcess();
	FexProcess(const FexProcess &) = delete;
	FexProcess &operator=(const FexProcess &) = delete;
	NativeResult call(uint64_t address, std::span<const uint64_t> arguments);
	NativeResult resume();
	NativeContext context() const;
	void set_context(const NativeContext &context);
	// NativeProcess forwarding surface. Unsupported capabilities fail explicitly.
	NativeResult call(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout);
	NativeResult callback(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout);
	NativeResult resume(std::chrono::milliseconds timeout);
	std::vector<uint8_t> read_memory(uint64_t address, size_t size) const;
	void write_memory(uint64_t address, std::span<const uint8_t> bytes);
	void validate_write_memory(uint64_t address, size_t size) const;
	uint64_t compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size);
	uint64_t map_memory(size_t size, int protection, uint64_t address);
	void protect_memory(uint64_t address, size_t size, int protection);
	bool has_protection(uint64_t address, size_t size, int protection) const;
	void protect_image(uint64_t address, size_t size, int protection);
	void unmap_memory(uint64_t address);
	uint64_t map_shared(int descriptor, size_t size, uint64_t offset);
	uint64_t map_descriptor(int descriptor, size_t size, uint64_t offset);
	bool memory_available(uint64_t address, size_t size) const;
	void discard_memory(uint64_t address, size_t size);
	void map_image(LoadedImage &image);
	void unmap_image(uint64_t address);
	std::pair<uint64_t, uint64_t> stack_bounds() const;
	bool active() const;
	void abandon_call();
	void end_call();
	void add_api_trap(uint64_t address, uint32_t callee_cleanup);
	NativeMode mode() const;
	void set_teb(uint64_t base);
	void enter_nt_service(uint64_t instruction, uint32_t number);
	void complete_debug_print(uint64_t instruction);
	uint64_t return_gate() const;
	uint64_t unix_call_gate() const;
	uint64_t load_unix_library(const std::string &path, uint32_t function_count);
	void drain_unix_events(const std::function<void(uint64_t)> &signal);
	uint32_t copy_unix_port(uint32_t guest_port) const;
	void set_unix_pump(std::function<void()> pump);
	size_t callback_depth() const;
	NativeContext callback_origin(size_t index) const;
	uint64_t result_register() const;
	void watch_write(uint64_t address);
	void trace_instructions(size_t count, std::function<void(uint64_t)> sink);
	NativeThreadState save_thread(bool snapshot_registers) const;
	void load_thread(const NativeThreadState &thread);
	NativeThreadState new_thread(uint64_t stack_top, uint64_t teb);
	bool parallel() const;
	bool stopped() const;
	int execution_descriptor() const;
	void drain_execution();
	void set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait);
	void stop_thread(const NativeThreadState &thread);
	void pause_thread(NativeThreadState &thread);
	void enter_fault_dispatch();
	void redirect(const NativeContext &context, size_t depth);
	std::vector<uint64_t> api_arguments(size_t count) const;
	void set_api_argument(size_t index, uint32_t value);
	void set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield);
	void begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats);
	uint64_t end_translated_call();
	std::array<uint64_t, 4> api_float_arguments() const;
	void return_api(uint64_t value);
	void set_served_calls(uint64_t *block, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset, std::vector<uint64_t> yield_traps, std::function<void(uint64_t stub)> handler, std::vector<uint64_t> queued_stubs);
	uint64_t serving_teb() const;
	bool serving() const;
	void decline_served();
	bool serving_queued() const;
	void set_queued_gl_elements(bool available);
	void set_resume_hook(std::function<void()> hook);
	void set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t rip)> sampler);
private:
	struct State;
	std::unique_ptr<State> state;
};
void select_fex_execution(bool enabled);
bool fex_execution_selected();
bool fex_address_available(uint64_t address, uint64_t size);
NativeResult run_fex_entry(const PEImage &image, LoadedImage &loaded, const std::string &root);
}
