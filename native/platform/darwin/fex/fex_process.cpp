// SPDX-License-Identifier: MIT
#include "fex_process.h"
#define RAVEN_FEX_EMBEDDED
#include "modhost/native_host.cpp"
#include <pthread.h>
#include <mach/mach_vm.h>
#include <set>
#include <map>
#include <sys/mman.h>
#include <atomic>
#include <condition_variable>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {
// The frontend has no qualified detach/reinitialization protocol. Keep the PE
// mapping through static teardown; the OS releases it at host-process exit.
Mapping *const module_mapping = new Mapping{MAP_FAILED, 0};
bool module_claimed = false;
struct PendingImages {
	std::map<uint64_t, size_t> ranges;
	~PendingImages() { for (auto [base, size] : ranges) munmap(reinterpret_cast<void *>(base), size); }
} pending_images;
bool selected_fex = false;
thread_local void *guest_teb = nullptr;
thread_local sigjmp_buf stop_frame;
struct StopPacket {
	raven::NativeStop kind;
	uint64_t value, rip;
	RavenFault::Record exception;
};
thread_local StopPacket last_stop{};
constexpr uint32_t completion = 0x7ffffffe;
uint64_t packet_word(void *packet, size_t offset)
{
	uint64_t value;
	memcpy(&value, static_cast<uint8_t *>(packet) + offset, 8);
	return value;
}
uint64_t process_dispatch(uint64_t number, void *packet)
{
	auto rip = packet_word(packet, 0xf8);
	last_stop = {number == completion ? raven::NativeStop::returned : raven::NativeStop::syscall,
		number == completion ? packet_word(packet, 0xc8) : number, rip, {}};
	siglongjmp(stop_frame, 1);
}
int32_t process_exception(void *opaque, void *packet, void *)
{
	RavenFault::alignment_recovery = nullptr;
	auto &record = *static_cast<RavenFault::Record *>(opaque);
	last_stop = {record.code == 0x102 ? raven::NativeStop::timeout : raven::NativeStop::exception, record.code, packet_word(packet, 0xf8), record};
	siglongjmp(stop_frame, 1);
}
uint32_t guest_protection(int protection)
{
	if (protection & PROT_EXEC) return protection & PROT_WRITE ? 0x40 : 0x20;
	return protection & PROT_WRITE ? 4 : protection & PROT_READ ? 2 : 1;
}
void notify_memory(const char *name, uint64_t *args, size_t after, const std::function<void()> &change = {})
{
	// The module's public notification protocol brackets its tracker mutex.
	// New allocations are notified before publication to the guest. Protection
	// and release operations also bracket the actual mapping change below.
	args[after] = 0;
	raven_ec_call(mapped_image->export_address(name), args, thread_environment->teb.data());
	try {
		if (change) change();
	} catch (...) {
		args[after] = 1;
		args[after + 1] = 0xc0000001;
		raven_ec_call(mapped_image->export_address(name), args, thread_environment->teb.data());
		throw;
	}
	args[after] = 1;
	raven_ec_call(mapped_image->export_address(name), args, thread_environment->teb.data());
}
void initialize_process(Bytes bytes, size_t opt)
{
	initialize_image(bytes, opt, process_dispatch, process_exception);
}
}

namespace raven {
// Workers execute only translated code. The existing NT scheduler retains all API
// handlers on its controller/coroutine stacks, just as with the Rosetta agent.
struct FexWorker {
	std::mutex mutex;
	std::condition_variable wake;
	std::thread host;
	std::unique_ptr<RavenNT::ThreadEnvironment> environment;
	std::exception_ptr failure;
	StopPacket result{};
	bool ready = false, requested = false, quit = false;
	std::atomic<bool> stopped{true};
	uint64_t teb = 0;
	int descriptor;
	explicit FexWorker(int fd) : descriptor(fd) {
		host = std::thread([this] {
			AlternateSignalStack alternate;
			try {
				environment = std::make_unique<RavenNT::ThreadEnvironment>(*process_environment);
				RavenNT::active_thread = environment.get();
				mapped_image->install_tls(*environment);
				uint64_t args[8]{};
				if (raven_ec_call(mapped_image->export_address("ThreadInit"), args, environment->teb.data()))
					throw std::runtime_error("FEX worker ThreadInit failed");
				std::unique_lock lock(mutex);
				ready = true;
				wake.notify_all();
				for (;;) {
					wake.wait(lock, [this] { return requested || quit; });
					if (quit) break;
					requested = false;
					guest_teb = teb ? reinterpret_cast<void *>(teb) : environment->teb.data();
					RavenFault::execution_teb = guest_teb;
					lock.unlock();
					auto previous = NativeJit::protect(1);
					if (!sigsetjmp(stop_frame, 1)) {
						raven_ec_call(mapped_image->export_address("BeginSimulation"), args, guest_teb);
						std::abort();
					}
					asm volatile("mov x18,xzr");
					NativeJit::protect(previous);
					lock.lock();
					result = last_stop;
					stopped.store(true, std::memory_order_release);
					wake.notify_all();
					uint64_t one = 1;
					(void)::write(descriptor, &one, sizeof(one));
				}
				lock.unlock();
				// The public module frontend tears down the current FEX state without
				// terminating the pthread. Use its private TEB/identity for this call.
				RavenFault::execution_teb = nullptr;
				args[0] = uint64_t(-2);
				if (raven_ec_call(mapped_image->export_address("ThreadTerm"), args, environment->teb.data()))
					throw std::runtime_error("FEX worker ThreadTerm failed");
			} catch (...) {
				std::lock_guard lock(mutex);
				failure = std::current_exception();
				ready = true;
				stopped.store(true, std::memory_order_release);
				wake.notify_all();
			}
			RavenNT::active_thread = nullptr;
		});
		std::unique_lock lock(mutex);
		wake.wait(lock, [this] { return ready; });
		if (failure) { lock.unlock(); host.join(); std::rethrow_exception(failure); }
	}
	void pause() {
		if (!stopped.load(std::memory_order_acquire)) {
			__atomic_store_n(environment->cpu.doorbell, 1u, __ATOMIC_RELEASE);
			std::unique_lock lock(mutex);
			wake.wait(lock, [this] { return stopped.load(std::memory_order_acquire); });
		}
	}
	void shutdown() {
		if (!host.joinable()) return;
		pause();
		{ std::lock_guard lock(mutex); quit = true; }
		wake.notify_all();
		if (host.joinable()) host.join();
		if (failure) std::rethrow_exception(failure);
	}
	~FexWorker() {
		try { shutdown(); }
		catch (const std::exception &error) {
			std::cerr << error.what() << "\n";
			std::abort(); // Teardown failure must not masquerade as guest success.
		}
	}
};
struct FexProcess::State {
	pthread_t owner = pthread_self();
	bool parallel = std::getenv("RAVEN_PARALLEL") != nullptr;
	int execution = -1;
	pid_t next_worker = 1, selected = 0;
	std::map<pid_t, std::unique_ptr<FexWorker>> workers;
	std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> execution_wait;
	RavenNT::ThreadEnvironment *environment() const {
		return parallel ? workers.at(selected)->environment.get() : thread_environment.get();
	}
	pid_t make_worker() {
		auto id = next_worker++;
		workers.emplace(id, std::make_unique<FexWorker>(execution));
		return id;
	}
	struct Region { size_t size; bool image; };
	std::map<uint64_t, Region> regions;
	std::map<uint64_t, int> pages;
	std::set<uint64_t> traps;
	std::map<uint64_t, size_t> image_reservations;
	std::vector<LoadedImage *> images;
	NativeThreadState thread;
	NativeContext initial;
	std::function<void()> resume_hook;
	uint64_t teb = 0;
	uint64_t stack_top = 0;
	std::function<void()> unix_pump;
	std::chrono::milliseconds budget{5000}, slice{0};
	std::function<bool()> slice_wanted;
	std::function<void()> slice_yield;
	void *stack = MAP_FAILED, *gate = MAP_FAILED;
	size_t stack_size = 1024 * 1024;
	std::unique_ptr<RavenFault::Scope> faults;
	bool active = false;
	State() = default;
	~State()
	{
		workers.clear();
		if (execution >= 0) ::close(execution);
		faults.reset();
		// Mapping is a simple scope owner; the vector entries are released here.
		if (stack != MAP_FAILED) munmap(stack, stack_size);
		if (gate != MAP_FAILED) munmap(gate, 16384);
		for (auto [base, size] : image_reservations) munmap(reinterpret_cast<void *>(base), size);
		for (auto [base, region] : regions) munmap(reinterpret_cast<void *>(base), region.size);
	}
	void require_owner() const
	{
		if (!pthread_equal(owner, pthread_self()))
			throw std::runtime_error("FEX process used from another host thread");
	}
};
FexProcess::FexProcess(std::span<LoadedImage *const> images, const std::string &root, uint64_t stack_reserve)
	: state(std::make_unique<State>())
{
	state->stack_size = (std::clamp<uint64_t>(stack_reserve, 256 * 1024, 64 * 1024 * 1024) + 16383) & ~uint64_t(16383);
	if (module_claimed)
		throw std::runtime_error("FEX module already claimed; a second FexProcess in this host is unsupported");
	if (root.empty())
		throw std::runtime_error("FEX requires RAVEN_FEX_MODULE_ROOT");
	// Reserve initial preferred/fallback bases before FEX/dyld initialization
	// can claim them. map_image replaces only these owned reservations.
	for (auto *image : images) {
		const auto n = (image->data().size() + 16383) & ~size_t(16383);
		mach_vm_address_t base = image->base();
		auto reserved = pending_images.ranges.find(base);
		if (reserved != pending_images.ranges.end() && reserved->second == n)
			pending_images.ranges.erase(reserved);
		else if (mach_vm_allocate(mach_task_self(), &base, n, VM_FLAGS_FIXED) != KERN_SUCCESS)
			throw std::runtime_error("FEX guest image reservation conflict at " + std::to_string(image->base()));
		state->image_reservations.emplace(base, n);
	}
	module_claimed = true;
	std::string path = root + "/xtajit64.dll";
	char program[] = "raven-fex";
	char *argv[]{program, path.data()};
	if (map_module(2, argv, resolve, inspect_image, initialize_process, module_mapping))
		throw std::runtime_error("FEX module initialization failed");
	// Guest x86 code is data to the arm64 host. Keep it non-executable; only FEX's
	// separate native code cache uses MAP_JIT. Never replace an existing host mapping.
	for (auto *image : images) map_image(*image);
	state->stack = mmap(nullptr, state->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	state->gate = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (state->stack == MAP_FAILED || state->gate == MAP_FAILED)
		throw std::runtime_error("FEX guest stack/gate allocation failed");
	// mov r10,rax; mov eax,completion; syscall; ud2
	uint8_t gate[]{0x49, 0x89, 0xc2, 0xb8, 0xfe, 0xff, 0xff, 0x7f, 0x0f, 0x05, 0x0f, 0x0b};
	memcpy(state->gate, gate, sizeof(gate));
	uint64_t args[8]{uintptr_t(state->gate), 16384, 0x3000, 0x40, 1, 0};
	notify_memory("NotifyMemoryAlloc", args, 4);
	state->stack_top = uintptr_t(state->stack) + state->stack_size;
	state->thread.stack_top = state->stack_top;
	for (uint64_t at = uintptr_t(state->stack); at < state->stack_top; at += 4096) state->pages[at] = PROT_READ | PROT_WRITE;
	state->pages[uintptr_t(state->gate)] = PROT_READ | PROT_EXEC;
	if (state->parallel) {
		state->execution = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (state->execution < 0) throw std::runtime_error("FEX execution eventfd failed");
		state->selected = state->make_worker();
		state->thread.task = state->selected;
	}
	state->initial = context();
	state->faults = std::make_unique<RavenFault::Scope>(mapped_image->export_address("RavenHandleFault"));
}
FexProcess::~FexProcess() = default;
NativeResult FexProcess::call(uint64_t address, std::span<const uint64_t> arguments)
{
	state->require_owner();
	if (state->active || arguments.size() > 128)
		throw std::runtime_error("FEX entry requires an idle frame and at most 128 arguments");
	auto *packet = state->environment()->context.data();
	// Preserve ThreadInit's segment selectors and floating-point defaults.
	memset(packet + 0x78, 0, 17 * 8);
	auto rsp = (state->thread.stack_top - 32 - std::max<size_t>(arguments.size(), 4) * 8) & ~uint64_t(15);
	rsp -= 8;
	auto gate = uintptr_t(state->gate);
	memcpy(reinterpret_cast<void *>(rsp), &gate, 8);
	memcpy(packet + 0x98, &rsp, 8);
	memcpy(packet + 0xf8, &address, 8);
	constexpr size_t offsets[]{0x80, 0x88, 0xb8, 0xc0};
	for (size_t i = 0; i < arguments.size(); ++i) {
		if (i < 4) memcpy(packet + offsets[i], &arguments[i], 8);
		else memcpy(reinterpret_cast<void *>(rsp + 40 + (i - 4) * 8), &arguments[i], 8);
	}
	state->active = true;
	state->thread.active_call = true;
	state->thread.awaiting_api = false;
	return resume(state->budget);
}
NativeResult FexProcess::resume()
{
	state->require_owner();
	if (!state->active || state->thread.awaiting_api)
		throw std::runtime_error("FEX has no resumable call or awaits an API result");
	state->thread.awaiting_syscall = state->thread.debug_print_fault = false;
	if (state->resume_hook) state->resume_hook();
	if (state->parallel) {
		auto *worker = state->workers.at(state->selected).get();
		const auto deadline = std::chrono::steady_clock::now() + state->budget;
		{
			std::lock_guard lock(worker->mutex);
			worker->teb = state->teb;
			__atomic_store_n(worker->environment->cpu.doorbell, 0u, __ATOMIC_RELEASE);
			worker->stopped.store(false, std::memory_order_release);
			worker->requested = true;
		}
		worker->wake.notify_all();
		auto ready = [worker] { return worker->stopped.load(std::memory_order_acquire); };
		struct PauseOnExit {
			FexWorker *worker;
			~PauseOnExit() { worker->pause(); }
		} pause_on_exit{worker};
		if (state->execution_wait) {
			state->execution_wait(ready, deadline);
		} else {
			std::unique_lock lock(worker->mutex);
			worker->wake.wait_until(lock, deadline, ready);
		}
		worker->pause();
		last_stop = worker->result;
	} else {
		std::mutex timer_mutex;
		std::condition_variable_any timer_wake;
		auto *doorbell = state->environment()->cpu.doorbell;
		std::jthread timer([&](std::stop_token stop) {
			std::unique_lock lock(timer_mutex);
			timer_wake.wait_for(lock, stop, state->budget, [] { return false; });
			if (!stop.stop_requested()) __atomic_store_n(doorbell, 1u, __ATOMIC_RELEASE);
		});
		uint64_t args[8]{};
		auto previous = NativeJit::protect(1);
		if (!sigsetjmp(stop_frame, 1)) {
			raven_ec_call(mapped_image->export_address("BeginSimulation"), args, guest_teb ? guest_teb : state->environment()->teb.data());
			std::abort();
		}
		timer.request_stop();
		timer_wake.notify_all();
		timer.join();
		__atomic_store_n(doorbell, 0u, __ATOMIC_RELEASE);
		asm volatile("mov x18,xzr");
		NativeJit::protect(previous);
	}
	if (std::getenv("RAVEN_FEX_TRACE"))
		std::cerr << "FEX stop rip=" << std::hex << last_stop.rip << " address=" << last_stop.exception.information[1] << std::dec << "\n";
	if (last_stop.kind == NativeStop::returned)
		state->active = false;
	NativeResult result{last_stop.kind, last_stop.value, last_stop.rip,
		last_stop.kind == NativeStop::exception ? SIGSEGV : 0};
	state->thread.debug_print_fault = false;
	if (last_stop.kind == NativeStop::exception) {
		auto &record = last_stop.exception;
		// INT 2D debug-print handling is restricted to the canonical provider body.
		if (!state->traps.empty() && result.value == 0x80000003 && record.flags == 0 &&
			record.count == 1 && record.information[0] == 1 && context().registers.rax == 1 &&
			result.instruction >= 2 && !state->traps.contains(result.instruction - 2) && has_protection(result.instruction - 2, 4, PROT_READ | PROT_EXEC) &&
			read_memory(result.instruction - 2, 4) == std::vector<uint8_t>{0xcd, 0x2d, 0xcc, 0xc3}) {
			// FEX reports INT 2D after its two-byte instruction. Normalize only
			// the canonical service that the provider can complete as a fault.
			auto saved = context();
			result.instruction -= 2;
			saved.registers.rip = result.instruction;
			set_context(saved);
			state->thread.debug_print_fault = true;
		}
		result.exception_flags = record.flags;
		if (record.count > 15) throw std::runtime_error("FEX exception information exceeds packet");
		result.exception_information.assign(record.information, record.information + record.count);
		if (record.count > 1) result.fault_address = record.information[1];
	}
	if (result.stop == NativeStop::exception && result.value == 0x80000003 && state->traps.contains(result.instruction)) {
		auto saved = context();
		saved.registers.rip += 1;
		set_context(saved);
		state->thread.awaiting_api = true;
		result.stop = NativeStop::api;
		result.value = result.instruction;
		result.instruction += 1;
	}
	if (!state->traps.empty() && result.stop == NativeStop::exception) {
		result.stop = NativeStop::fault;
		result.exception_code = uint32_t(result.value);
		result.signal = result.value == 0xc000001d ? SIGILL : result.value == 0xc0000094 ? SIGFPE :
			result.value == 0x80000003 ? SIGTRAP : SIGSEGV;
		if (result.signal == SIGFPE) result.signal_code = FPE_INTDIV;
		if (state->thread.debug_print_fault) {
			result.value = 1;
			result.signal = SIGSEGV;
			result.signal_code = SI_KERNEL;
		}
	}
	if (result.stop == NativeStop::syscall) {
		state->thread.awaiting_syscall = true;
		state->thread.context.registers.orig_rax = result.value;
	}
	if (result.stop == NativeStop::returned) {
		state->thread.active_call = false;
		if (!state->thread.callbacks.empty()) {
			auto outer = state->thread.callbacks.back();
			state->thread.callbacks.pop_back();
			outer.xstate = context().xstate;
			set_context(outer);
			state->active = state->thread.active_call = true;
			state->thread.awaiting_api = true;
		}
	}
	return result;
}
NativeContext FexProcess::context() const
{
	state->require_owner();
	if (!stopped()) throw std::runtime_error("FEX context requires a stopped worker");
	NativeContext result{};
	auto &r = result.registers;
	auto *p = state->environment()->context.data();
	unsigned long long *registers[]{&r.rax, &r.rcx, &r.rdx, &r.rbx, &r.rsp, &r.rbp, &r.rsi, &r.rdi,
		&r.r8, &r.r9, &r.r10, &r.r11, &r.r12, &r.r13, &r.r14, &r.r15, &r.rip};
	for (size_t i = 0; i < 17; ++i)
		*registers[i] = packet_word(p, 0x78 + i * 8);
	r.orig_rax = state->thread.awaiting_syscall ? state->thread.context.registers.orig_rax : uint64_t(-1);
	r.gs_base = state->teb;
	r.eflags = *reinterpret_cast<uint32_t *>(p + 0x44);
	result.xstate.assign(p + 0x100, p + 0x300);
	return result;
}
void FexProcess::set_context(const NativeContext &value)
{
	state->require_owner();
	if (!stopped()) throw std::runtime_error("FEX context update requires a stopped worker");
	const auto &r = value.registers;
	if (r.fs_base) throw std::runtime_error("FEX x64 FS base is unsupported");
	if (r.gs_base) {
		validate_write_memory(r.gs_base + 0x1788, 8);
		auto cpu = uintptr_t(&state->environment()->cpu);
		memcpy(reinterpret_cast<void *>(r.gs_base + 0x1788), &cpu, 8);
		uint64_t peb = 0;
		memcpy(&peb, reinterpret_cast<void *>(r.gs_base + 0x60), 8);
		if (peb) {
			validate_write_memory(peb + 0x368, 8);
			auto bitmap = process_environment->code_bitmap;
			memcpy(reinterpret_cast<void *>(peb + 0x368), &bitmap, 8);
		}
	}
	state->teb = r.gs_base;
	guest_teb = state->teb ? reinterpret_cast<void *>(state->teb) : nullptr;
	RavenFault::execution_teb = guest_teb;
	if (!value.xstate.empty() && value.xstate.size() != 512)
		throw std::runtime_error("FEX requires a 512-byte legacy floating-point packet");
	const uint64_t registers[]{r.rax, r.rcx, r.rdx, r.rbx, r.rsp, r.rbp, r.rsi, r.rdi,
		r.r8, r.r9, r.r10, r.r11, r.r12, r.r13, r.r14, r.r15, r.rip};
	auto *p = state->environment()->context.data();
	memcpy(p + 0x78, registers, sizeof(registers));
	uint32_t flags = uint32_t(r.eflags);
	memcpy(p + 0x44, &flags, 4);
	if (!value.xstate.empty()) memcpy(p + 0x100, value.xstate.data(), 512);
}
NativeResult run_fex_entry(const PEImage &image, LoadedImage &loaded, const std::string &root)
{
	if (!image.is_64 || (image.characteristics & 0x2000) || !image.imports().empty() ||
		image.directories[9].size || image.directories[13].size || image.directories[14].size)
		throw std::runtime_error("FEX entry requires PE64 without imports, TLS, delayed imports or CLR");
	LoadedImage *images[]{&loaded};
	FexProcess process(images, root);
	return process.call(loaded.base() + image.entry, {});
}
}

namespace raven {
void select_fex_execution(bool enabled) { selected_fex = enabled; }
bool fex_execution_selected() { return selected_fex; }
bool fex_address_available(uint64_t address, uint64_t size)
{
	if (!size || address < 0x100000000ULL || address > UINT64_MAX - size) return false;
	mach_vm_address_t base = address;
	mach_vm_size_t length = 0;
	vm_region_basic_info_data_64_t info{};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto status = mach_vm_region(mach_task_self(), &base, &length, VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	if (object) mach_port_deallocate(mach_task_self(), object);
	if (std::getenv("RAVEN_FEX_TRACE_MEMORY")) {
		static size_t calls = 0;
		if (++calls < 8 || !(calls % 1000)) std::cerr << "FEX available " << std::hex << address << "+" << size << " next=" << base << "+" << length << " status=" << status << std::dec << "\n";
	}
	return status == KERN_INVALID_ADDRESS || (status == KERN_SUCCESS && base >= address + size);
}
bool fex_reserve_image_address(uint64_t address, uint64_t size)
{
	// Loader preflight must claim the range immediately: even host allocation
	// during PE preparation can otherwise occupy a free preferred base.
	const auto n = (size + 16383) & ~uint64_t(16383);
	if (auto it = pending_images.ranges.find(address); it != pending_images.ranges.end())
		return it->second == n;
	mach_vm_address_t base = address;
	if (mach_vm_allocate(mach_task_self(), &base, n, VM_FLAGS_FIXED) != KERN_SUCCESS) return false;
	pending_images.ranges.emplace(address, n);
	return true;
}
bool FexProcess::memory_available(uint64_t address, size_t size) const { return fex_address_available(address, size); }
std::vector<uint8_t> FexProcess::read_memory(uint64_t address, size_t size) const
{
	state->require_owner();
	if (address > UINT64_MAX - size) throw std::runtime_error("FEX read range overflow");
	for (auto at = address & ~uint64_t(4095); at < address + size; at += 4096)
		if (!state->pages.contains(at) || !(state->pages.at(at) & PROT_READ))
			throw std::runtime_error("FEX read exceeds readable guest memory at " + std::to_string(address));
	auto *p = address >= 0x7ffe0000 && address + size <= 0x7ffe1000 ? user_shared_data.data() + address - 0x7ffe0000 : reinterpret_cast<const uint8_t *>(address);
	return {p, p + size};
}
void FexProcess::validate_write_memory(uint64_t address, size_t size) const
{
	if (address > UINT64_MAX - size) throw std::runtime_error("FEX write range overflow");
	for (auto at = address & ~uint64_t(4095); at < address + size; at += 4096)
		if (!state->pages.contains(at) || !(state->pages.at(at) & PROT_WRITE))
			throw std::runtime_error("FEX write exceeds writable guest memory at " + std::to_string(address));
}
void FexProcess::write_memory(uint64_t address, std::span<const uint8_t> bytes)
{
	validate_write_memory(address, bytes.size());
	auto *p = address >= 0x7ffe0000 && address + bytes.size() <= 0x7ffe1000 ? user_shared_data.data() + address - 0x7ffe0000 : reinterpret_cast<uint8_t *>(address);
	memcpy(p, bytes.data(), bytes.size());
	uint64_t args[8]{address, bytes.size()};
	raven_ec_call(mapped_image->export_address("BTCpu64NotifyMemoryDirty"), args, state->environment()->teb.data());
}
uint64_t FexProcess::map_memory(size_t size, int protection, uint64_t address)
{
	state->require_owner();
	if (!size || size > SIZE_MAX - 65536) throw std::runtime_error("FEX invalid allocation size");
	if (address == 0x7ffe0000 && size == 4096) {
		memset(user_shared_data.data(), 0, user_shared_data.size());
		state->pages[address] = protection;
		return address;
	}
	const auto n = (size + 16383) & ~size_t(16383);
	if (address) {
		mach_vm_address_t base = address;
		if (address % 16384 || mach_vm_allocate(mach_task_self(), &base, n, VM_FLAGS_FIXED) != KERN_SUCCESS)
			throw std::runtime_error("FEX fixed allocation unavailable");
	} else {
		auto *p = mmap(nullptr, n + 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (p == MAP_FAILED) throw std::runtime_error("FEX allocation failed");
		const auto begin = uintptr_t(p);
		address = (begin + 65535) & ~uint64_t(65535);
		if (address > begin) munmap(p, address - begin);
		if (begin + n + 65536 > address + n) munmap(reinterpret_cast<void *>(address + n), begin + n + 65536 - address - n);
	}
	state->regions.emplace(address, State::Region{n, false});
	uint64_t args[8]{address, n, 0x3000, guest_protection(protection), 0, 0};
	notify_memory("NotifyMemoryAlloc", args, 4);
	for (size_t at = 0; at < n; at += 4096) state->pages[address + at] = PROT_READ | PROT_WRITE;
	protect_memory(address, n, protection);
	return address;
}
void FexProcess::protect_memory(uint64_t address, size_t size, int protection)
{
	if (!size || address % 4096 || size % 4096 || address > UINT64_MAX - size)
		throw std::runtime_error("FEX invalid protection range");
	for (uint64_t at = address; at < address + size; at += 4096)
		if (!state->pages.contains(at)) throw std::runtime_error("FEX protection range is unmapped");
	uint64_t args[8]{address, size, guest_protection(protection)};
	notify_memory("NotifyMemoryProtect", args, 3, [&] {
		for (uint64_t at = address; at < address + size; at += 4096) state->pages[at] = protection;
		if (address == 0x7ffe0000 && size == 4096) return;
		// Tier 0: union the four logical protections of each real 16K page. Guest
		// executable bytes remain host data; MAP_JIT owns native executable code.
		for (auto page = address & ~uint64_t(16383); page < address + size; page += 16384) {
			int combined = 0;
			for (uint64_t at = page; at < page + 16384; at += 4096) {
				const auto found = state->pages.find(at);
				if (found != state->pages.end()) combined |= found->second;
			}
			combined = (combined & (PROT_READ | PROT_WRITE)) | (combined & PROT_EXEC ? PROT_READ : 0);
			if (mprotect(reinterpret_cast<void *>(page), 16384, combined)) throw std::runtime_error("FEX host protection failed");
		}
	});
}
void FexProcess::protect_image(uint64_t address, size_t size, int protection) { protect_memory(address, size, protection); }
bool FexProcess::has_protection(uint64_t address, size_t size, int protection) const
{
	if (!size || address > UINT64_MAX - size) return false;
	for (auto at = address & ~uint64_t(4095); at < address + size; at += 4096) {
		auto found = state->pages.find(at);
		if (found == state->pages.end() || found->second != protection) return false;
	}
	return true;
}
void FexProcess::unmap_memory(uint64_t address)
{
	auto found = state->regions.find(address);
	if (found == state->regions.end()) throw std::runtime_error("FEX allocation not found");
	const auto n = found->second.size;
	uint64_t args[8]{address, n, 0x8000};
	notify_memory("NotifyMemoryFree", args, 3, [&] {
		if (munmap(reinterpret_cast<void *>(address), n)) throw std::runtime_error("FEX unmap failed");
	});
	for (auto at = address; at < address + n; at += 4096) state->pages.erase(at);
	state->regions.erase(found);
}
void FexProcess::unmap_image(uint64_t address)
{
	std::erase_if(state->images, [&](auto *image) { return image->base() == address; });
	unmap_memory(address);
}
void FexProcess::map_image(LoadedImage &image)
{
	const auto address = image.base();
	const auto n = (image.data().size() + 16383) & ~size_t(16383);
	mach_vm_address_t base = address;
	if (!state->image_reservations.contains(address) &&
		mach_vm_allocate(mach_task_self(), &base, n, VM_FLAGS_FIXED) != KERN_SUCCESS)
		throw std::runtime_error("FEX guest image address conflict at " + std::to_string(address));
	auto *p = mmap(reinterpret_cast<void *>(address), n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, image.descriptor(), 0);
	state->image_reservations.erase(address);
	if (p == MAP_FAILED) { munmap(reinterpret_cast<void *>(address), n); throw std::runtime_error("FEX shared image mapping failed"); }
	state->regions.emplace(address, State::Region{n, true});
	state->images.push_back(&image);
	for (size_t at = 0; at < n; at += 4096) state->pages[address + at] = PROT_NONE;
	for (const auto &mapping : image.mappings())
		for (uint64_t at = mapping.address; at < mapping.address + mapping.size; at += 4096) state->pages[at] = mapping.protection;
	// Register the view before changing protections; FEX tracks executable pages.
	uint64_t args[8]{address, n, 0x3000, 0x40, 1, 0};
	notify_memory("NotifyMemoryAlloc", args, 4);
	for (size_t at = 0; at < n; at += 4096) protect_memory(address + at, 4096, state->pages.at(address + at));
}
std::pair<uint64_t, uint64_t> FexProcess::stack_bounds() const { return {uintptr_t(state->stack), state->stack_top}; }
NativeMode FexProcess::mode() const { return NativeMode::x64; }
bool FexProcess::active() const { return state->active; }
bool FexProcess::stopped() const { return !state->parallel || state->workers.at(state->selected)->stopped.load(std::memory_order_acquire); }
bool FexProcess::parallel() const { return state->parallel; }
uint64_t FexProcess::return_gate() const { return uintptr_t(state->gate); }
uint64_t FexProcess::unix_call_gate() const { return 0; } // No same-process unix-library gate in M2.
void FexProcess::add_api_trap(uint64_t address, uint32_t callee_cleanup)
{
	if (callee_cleanup) throw std::runtime_error("FEX x64 API cannot use stdcall cleanup");
	state->traps.insert(address);
}
size_t FexProcess::callback_depth() const { return state->thread.callbacks.size(); }
NativeContext FexProcess::callback_origin(size_t index) const { return state->thread.callbacks.at(index); }
uint64_t FexProcess::result_register() const { return context().registers.rax; }
void FexProcess::set_resume_hook(std::function<void()> hook) { state->resume_hook = std::move(hook); }
std::vector<uint64_t> FexProcess::api_arguments(size_t count) const
{
	if (state->thread.translated_words) return *state->thread.translated_words;
	const auto r = context().registers;
	std::vector<uint64_t> words;
	for (size_t i = 0; i < count; ++i) {
		if (i < 4) { const uint64_t registers[]{r.rcx, r.rdx, r.r8, r.r9}; words.push_back(registers[i]); }
		else { auto bytes = read_memory(r.rsp + 40 + (i - 4) * 8, 8); uint64_t value; memcpy(&value, bytes.data(), 8); words.push_back(value); }
	}
	return words;
}
std::array<uint64_t, 4> FexProcess::api_float_arguments() const
{
	std::array<uint64_t, 4> values{};
	const auto saved = context();
	for (size_t i = 0; i < 4; ++i) memcpy(&values[i], saved.xstate.data() + 160 + i * 16, 8);
	return values;
}
void FexProcess::enter_nt_service(uint64_t instruction, uint32_t number)
{
	state->require_owner();
	if (!stopped() || !state->active || state->thread.awaiting_api || !state->thread.awaiting_syscall)
		throw std::runtime_error("FEX is not at a pending syscall service");
	auto saved = context();
	if (saved.registers.rip != instruction || saved.registers.orig_rax != number || instruction < 2 ||
		!has_protection(instruction - 2, 3, PROT_READ | PROT_EXEC) ||
		read_memory(instruction - 2, 3) != std::vector<uint8_t>{0x0f, 0x05, 0xc3})
		throw std::runtime_error("FEX NT service boundary does not match the captured syscall");
	saved.registers.rcx = saved.registers.r10;
	set_context(saved);
	state->thread.awaiting_syscall = false;
	state->thread.awaiting_api = true;
}
void FexProcess::complete_debug_print(uint64_t instruction)
{
	state->require_owner();
	const auto saved = context();
	if (!stopped() || !state->active || state->thread.awaiting_api || state->thread.awaiting_syscall ||
		!state->thread.debug_print_fault || saved.registers.rip != instruction || saved.registers.rax != 1 ||
		read_memory(instruction, 4) != std::vector<uint8_t>{0xcd, 0x2d, 0xcc, 0xc3})
		throw std::runtime_error("FEX debug print does not match a captured INT 2D service");
	state->thread.debug_print_fault = false;
	state->thread.awaiting_api = true;
	return_api(0);
}
void FexProcess::return_api(uint64_t value)
{
	if (state->thread.translated_words && callback_depth() == state->thread.translated_depth) { state->thread.translated_result = value; return; }
	if (!state->thread.awaiting_api) throw std::runtime_error("FEX has no pending API");
	auto saved = context();
	const auto bytes = read_memory(saved.registers.rsp, 8);
	memcpy(&saved.registers.rip, bytes.data(), 8);
	saved.registers.rsp += 8;
	saved.registers.rax = value;
	set_context(saved);
	state->thread.awaiting_api = false;
}
void FexProcess::end_call()
{
	if (state->parallel) state->workers.at(state->selected)->pause();
	state->active = state->thread.active_call = state->thread.awaiting_api = false;
	state->thread.awaiting_syscall = state->thread.debug_print_fault = false;
	state->thread.callbacks.clear();
}
void FexProcess::abandon_call() { end_call(); }
void FexProcess::enter_fault_dispatch() { state->thread.awaiting_api = true; }
void FexProcess::redirect(const NativeContext &saved, size_t depth)
{
	if (depth > callback_depth()) throw std::runtime_error("FEX invalid redirect callback depth");
	state->thread.callbacks.resize(depth);
	set_context(saved);
	state->thread.awaiting_api = false;
	state->active = state->thread.active_call = true;
}
NativeResult FexProcess::callback(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout)
{
	if (!state->active || !state->thread.awaiting_api) throw std::runtime_error("FEX callback requires stopped API frame");
	const auto outer = context();
	state->thread.callbacks.push_back(outer);
	const auto ceiling = state->thread.stack_top;
	state->thread.stack_top = outer.registers.rsp - 256;
	state->active = false;
	auto result = call(address, arguments, timeout);
	state->thread.stack_top = ceiling;
	return result;
}
NativeThreadState FexProcess::save_thread(bool snapshot) const
{
	auto saved = state->thread;
	saved.stopped = stopped();
	saved.context_in_task = state->parallel && !snapshot;
	if (saved.stopped) {
		saved.context = context();
	} else {
		saved.context.registers.gs_base = state->teb;
	}
	saved.active_call = state->active;
	return saved;
}
void FexProcess::load_thread(const NativeThreadState &saved)
{
	if (state->parallel) state->selected = saved.task;
	state->thread = saved;
	state->teb = saved.context.registers.gs_base;
	state->active = saved.active_call;
	if (saved.stopped && !saved.context_in_task) set_context(saved.context);
}
NativeThreadState FexProcess::new_thread(uint64_t stack_top, uint64_t teb)
{
	NativeThreadState thread;
	thread.stack_top = stack_top;
	if (state->parallel) thread.task = state->make_worker();
	thread.context = state->initial;
	thread.context.registers.gs_base = teb;
	return thread;
}
int FexProcess::execution_descriptor() const { return state->execution; }
void FexProcess::drain_execution()
{
	uint64_t value;
	while (state->execution >= 0 && ::read(state->execution, &value, sizeof(value)) > 0) {}
}
void FexProcess::set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait)
{
	state->execution_wait = std::move(wait);
}
void FexProcess::set_unix_pump(std::function<void()> pump) { state->unix_pump = std::move(pump); }
void FexProcess::drain_unix_events(const std::function<void(uint64_t)> &) {}
uint64_t FexProcess::serving_teb() const { return 0; }
bool FexProcess::serving() const { return false; }
bool FexProcess::serving_queued() const { return false; }
void FexProcess::set_queued_gl_elements(bool available) { if (available) throw std::runtime_error("FEX queued graphics unavailable"); }
void FexProcess::set_served_calls(uint64_t *, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset,
	std::vector<uint64_t> yield_traps, std::function<void(uint64_t)>, std::vector<uint64_t> queued)
{
	if (!queued.empty()) throw std::runtime_error("FEX queued served calls are unsupported");
	if (!yield_traps.empty() && (stub_size != 112 || wait_offset != 85))
		throw std::runtime_error("FEX served stub layout is unsupported");
	std::vector<uint8_t *> pointers;
	std::vector<uint64_t> local_stubs;
	for (auto trap : yield_traps) {
		// Providers also append non-served prelude fragments to `stubs` for the
		// Rosetta scheduler. Only actual served stubs have the canonical yield.
		if (trap < 109 || std::find(stubs.begin(), stubs.end(), trap - 109) == stubs.end())
			throw std::runtime_error("FEX served yield trap has no canonical stub");
		const auto stub = trap - 109;
		if (state->parallel) {
			const std::vector<uint8_t> local{0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x90, 0x90, 0x90};
			if (read_memory(stub, local.size()) != local)
				throw std::runtime_error("FEX parallel served stub encoding is unsupported");
			local_stubs.push_back(stub);
			continue;
		}
		const auto code = read_memory(stub, 7);
		if (code[0] != 0x4c || code[1] != 0x8b || code[2] != 0x15)
			throw std::runtime_error("FEX served stub encoding is unsupported");
		int32_t displacement;
		memcpy(&displacement, code.data() + 3, 4);
		const auto pointer = stub + 7 + uint64_t(int64_t(displacement));
		bool found = false;
		for (auto *image : state->images) {
			const auto offset = pointer - image->base();
			if (pointer >= image->base() && offset <= image->data().size() && image->data().size() - offset >= 8) {
				pointers.push_back(image->data().data() + offset);
				found = true;
				break;
			}
		}
		if (!found) throw std::runtime_error("FEX served pointer is outside provider storage");
	}
	// Validate every real stub before clearing any pointer. Guest execution then
	// reaches its registered API breakpoint with the original arguments intact.
	for (auto *pointer : pointers) memset(pointer, 0, 8);
	// The parallel provider replaces the null check with a GS load. FEX serves
	// APIs on the controller, so bypass that protocol at its canonical API trap.
	// The parameter is the trap offset (112), not the provider stride (128).
	// rel32 is relative to stub+5: 5+107=112; 109 is only the yield trap.
	const std::array<uint8_t, 12> fallback{0xe9, 107, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	for (auto stub : local_stubs) {
		bool found = false;
		for (auto *image : state->images)
			if (stub >= image->base() && stub - image->base() + fallback.size() <= image->data().size()) {
				memcpy(image->data().data() + stub - image->base(), fallback.data(), fallback.size());
				found = true;
				break;
			}
		if (!found) throw std::runtime_error("FEX served stub is outside provider storage");
	}
}
}

namespace raven {
NativeResult FexProcess::call(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout)
{
	if (timeout.count() <= 0) throw std::runtime_error("FEX execution requires positive timeout");
	state->budget = timeout;
	return call(address, arguments);
}
NativeResult FexProcess::resume(std::chrono::milliseconds timeout)
{
	if (timeout.count() <= 0) throw std::runtime_error("FEX execution requires positive timeout");
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;) {
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (remaining.count() <= 0) return {NativeStop::timeout, 0, context().registers.rip, 0};
		const bool slicing = !state->parallel && state->slice.count() > 0 && state->slice_wanted && state->slice_yield && state->slice_wanted();
		state->budget = slicing ? std::min(remaining, state->slice) : remaining;
		auto result = resume();
		if (state->parallel && result.stop == NativeStop::timeout && std::chrono::steady_clock::now() < deadline)
			continue; // A remote suspend consumed this doorbell; resume the edited packet.
		if (result.stop != NativeStop::timeout || !slicing || state->budget >= remaining) return result;
		state->slice_yield();
	}
}
void FexProcess::set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield)
{
	state->slice = slice;
	state->slice_wanted = std::move(wanted);
	state->slice_yield = std::move(yield);
}
uint64_t FexProcess::compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size)
{
	if (size != 4 && size != 8) throw std::runtime_error("FEX compare exchange width unsupported");
	validate_write_memory(address, size);
	if (address % size) throw std::runtime_error("FEX unaligned compare exchange unsupported");
	if (size == 8) __atomic_compare_exchange_n(reinterpret_cast<uint64_t *>(address), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	else if (size == 4) { uint32_t word = uint32_t(expected); __atomic_compare_exchange_n(reinterpret_cast<uint32_t *>(address), &word, uint32_t(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); expected = word; }
	else throw std::runtime_error("FEX compare exchange width unsupported");
	return expected;
}
void FexProcess::discard_memory(uint64_t address, size_t size)
{
	validate_write_memory(address, size);
	std::vector<uint8_t> zeros(size);
	write_memory(address, zeros);
}
void FexProcess::begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats)
{
	state->thread.translated_words = std::move(words); state->thread.translated_floats = floats;
	state->thread.translated_depth = callback_depth();
}
uint64_t FexProcess::end_translated_call()
{
	const auto value = state->thread.translated_result;
	state->thread.translated_words.reset();
	return value;
}
}

namespace raven {
uint64_t FexProcess::map_shared(int descriptor, size_t size, uint64_t offset)
{
	if (offset % 16384) throw std::runtime_error("FEX shared mapping offset must be host-page aligned");
	const auto address = map_memory(size, PROT_READ | PROT_WRITE, 0);
	const auto n = state->regions.at(address).size;
	void *mapped = mmap(reinterpret_cast<void *>(address), n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, descriptor, off_t(offset));
	if (mapped == MAP_FAILED) { unmap_memory(address); throw std::runtime_error("FEX shared descriptor mapping failed"); }
	return address;
}
uint64_t FexProcess::map_descriptor(int descriptor, size_t size, uint64_t offset) { return map_shared(descriptor, size, offset); }
}

namespace raven {
void FexProcess::stop_thread(const NativeThreadState &thread)
{
	if (state->parallel && thread.task && thread.task != state->selected) {
		auto found = state->workers.find(thread.task);
		if (found != state->workers.end()) {
			try {
				found->second->shutdown();
			} catch (const std::exception &error) {
				// The worker has joined; finishing a coroutine must not throw.
				std::cerr << "FEX stop_thread: " << error.what() << "\n";
			} catch (...) {
				std::cerr << "FEX stop_thread: unknown teardown failure\n";
			}
			state->workers.erase(found);
		}
	}
}
void FexProcess::pause_thread(NativeThreadState &thread)
{
	if (!state->parallel || thread.stopped) return;
	auto selected = state->selected;
	auto teb = state->teb;
	state->selected = thread.task;
	state->teb = thread.context.registers.gs_base;
	state->workers.at(thread.task)->pause();
	thread.context = context();
	thread.stopped = true;
	thread.context_in_task = false;
	state->selected = selected;
	state->teb = teb;
}
}
