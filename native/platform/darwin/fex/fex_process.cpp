// SPDX-License-Identifier: MIT
#include "../mullion/backend.h"
#include "fex_process.h"
#include "perf.h"
#include "guest_window.h"
#include "unix_ranges.h"
#include "../mtld3d/callbacks.h"
#include "fex-module-config.h"
#include <mach-o/loader.h>
static bool raven_unix_fault_pc(uintptr_t pc);
#define RAVEN_UNIX_PC_FILTER
#define RAVEN_FEX_EMBEDDED
#include "modhost/native_host.cpp"
#include <pthread.h>
#include <mach/mach_vm.h>
#include <libproc.h>
#include <set>
#include <map>
#include <sys/mman.h>
#include <atomic>
#include <condition_variable>
#include <sys/eventfd.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fstream>
#include "../guest/protocol.h"

namespace {
// The frontend has no qualified detach/reinitialization protocol. Keep the PE
// mapping through static teardown; the OS releases it at host-process exit.
Mapping *const module_mapping = new Mapping{MAP_FAILED, 0};
bool module_claimed = false;
// A permanent Mach-O reservation, shared by initial images and later fixed
// guest allocations. Never let host mmap/dlopen reuse its uncommitted pages.
struct ImagePool {
	uint64_t base = 0, size = 0;
	std::map<uint64_t, size_t> claims;
	bool contains(uint64_t address, uint64_t length) const {
		return size && address >= base && address - base < size && length <= size - (address - base);
	}
	bool available(uint64_t address, uint64_t length) const {
		for (auto [start, count] : claims)
			if (address < start + count && start < address + length) return false;
		return true;
	}
} image_pool;
bool reserve_guest_range(uint64_t address, size_t size)
{
	if (image_pool.contains(address, size)) {
		if (!image_pool.available(address, size)) return false;
		if (mprotect(reinterpret_cast<void *>(address), size, PROT_READ | PROT_WRITE)) return false;
		image_pool.claims.emplace(address, size);
		return true;
	}
	mach_vm_address_t base = address;
	return mach_vm_allocate(mach_task_self(), &base, size, VM_FLAGS_FIXED) == KERN_SUCCESS;
}
void release_guest_range(uint64_t address, size_t size)
{
	if (image_pool.contains(address, size)) {
		if (mmap(reinterpret_cast<void *>(address), size, PROT_NONE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
            std::cerr << "FEX failed to restore guest reservation at " << std::hex << address << std::dec << "\n";
            return; // Retain the claim so a failed release cannot be reused.
        }
		image_pool.claims.erase(address);
	} else munmap(reinterpret_cast<void *>(address), size);
}
struct PendingImages {
	std::map<uint64_t, size_t> ranges;
	~PendingImages() { for (auto [base, size] : ranges) release_guest_range(base, size); }
} pending_images;
bool selected_fex = false;
thread_local void *guest_teb = nullptr;
thread_local sigjmp_buf stop_frame;
// A guest stop may longjmp out of RavenHandleFault or a native unix callback.
// Restore the enclosing host scope, rather than retaining abandoned handler frames.
struct FaultDispatchState {
    unsigned depth = RavenFault::dispatch_depth;
    sigjmp_buf *alignment = RavenFault::alignment_recovery;
    sigjmp_buf *unix_recovery = raven_unix_recovery;
    void restore() const {
        RavenFault::dispatch_depth = depth;
        RavenFault::alignment_recovery = alignment;
        raven_unix_recovery = unix_recovery;
    }
};

struct StopPacket {
	raven::NativeStop kind;
	uint64_t value, rip;
	RavenFault::Record exception;
};
thread_local StopPacket last_stop{};
// Libraries and their callback state live until process exit, including GPU callbacks.
struct UnixLibrary { std::string path; void *image; int32_t (**table)(void *); uint32_t count; const raven_unix_contract *contract; };
std::vector<UnixLibrary> unix_libraries;
struct UnixCodeRange { uintptr_t begin, end; };
UnixCodeRange unix_code_ranges[512]{};
std::atomic<unsigned> unix_code_count{0};
void register_unix_code(void *function) {
    Dl_info info{};
    if (!dladdr(function, &info)) throw std::runtime_error("unix image identity unavailable");
    auto *header = static_cast<const mach_header_64 *>(info.dli_fbase);
    auto *command = reinterpret_cast<const load_command *>(header + 1);
    uintptr_t preferred = 0;
    for (unsigned i=0; i<header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto *segment = reinterpret_cast<const segment_command_64 *>(command);
            if (!segment->fileoff && segment->filesize) preferred=segment->vmaddr;
        }
        command=reinterpret_cast<const load_command *>(reinterpret_cast<const char *>(command)+command->cmdsize);
    }
    command=reinterpret_cast<const load_command *>(header+1);
    for (unsigned i=0; i<header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto *segment=reinterpret_cast<const segment_command_64 *>(command);
            if (segment->initprot & VM_PROT_EXECUTE) {
                auto n=unix_code_count.load();
                if (n == 512) throw std::runtime_error("too many unix code ranges");
                uintptr_t begin=uintptr_t(header)+segment->vmaddr-preferred;
                unix_code_ranges[n]={begin,begin+segment->vmsize};
                unix_code_count.store(n+1,std::memory_order_release);
            }
        }
        command=reinterpret_cast<const load_command *>(reinterpret_cast<const char *>(command)+command->cmdsize);
    }
}

// Native libraries and GPU callbacks retain this validator until process exit.
UnixRanges &unix_ranges = *new UnixRanges;
int unix_check_range(uint64_t address, uint64_t size, uint32_t write) { return unix_ranges.check(address, size, write); }
std::function<std::vector<uint8_t>(uint64_t, size_t)> unix_read_memory;
int (*read_native_buffer)(uint64_t, void *, size_t) = nullptr;
raven_control *const unix_control = new raven_control{};
thread_local uint32_t *wow64_unix_doorbell = nullptr;
const bool trace_unix = getenv("RAVEN_TRACE_UNIX") != nullptr;
constexpr uint32_t unix_service = 0x7ffffffd;
constexpr uint32_t completion = 0x7ffffffe;
uint64_t packet_word(void *packet, size_t offset)
{
	uint64_t value;
	memcpy(&value, static_cast<uint8_t *>(packet) + offset, 8);
	return value;
}
uint64_t process_dispatch(uint64_t number, void *packet)
{
	if (number == unix_service) {
		uint64_t handle = packet_word(packet, 0xc8); // R10, preserved RCX
		uint32_t ordinal = uint32_t(packet_word(packet, 0x88)); // EDX
		uint64_t args = packet_word(packet, 0xb8); // R8
		if (wow64_module) {
			uint32_t esp;
			memcpy(&esp, static_cast<uint8_t *>(packet) + 0xc4, 4);
			if (esp > UINT32_MAX - 20) return 0xc000000du;
			std::vector<uint8_t> words;
			try { words = unix_read_memory(uint64_t(esp) + 4, 16); }
			catch (const std::runtime_error &) { return 0xc0000005u; }
			memcpy(&handle, words.data(), 8);
			memcpy(&ordinal, words.data() + 8, 4);
			uint32_t guest_args;
			memcpy(&guest_args, words.data() + 12, 4);
			if (!guest_args) return 0xc0000005u;
			args = wow64_guest_base + guest_args;
		}
		uint64_t status = 0xc0000008u;
		if (handle && handle <= unix_libraries.size()) {
			const auto &library = unix_libraries[handle - 1];
			if (wow64_module && ordinal < library.count &&
				(args - wow64_guest_base > UINT32_MAX || library.contract->argument_sizes[ordinal] >
					(uint64_t(1) << 32) - (args - wow64_guest_base))) return 0xc0000005u;
			if (trace_unix) fprintf(stderr, "unix enter %u\n", ordinal);
			status = ordinal < library.count && library.table[ordinal] ? uint32_t(raven_unix_invoke(library.table[ordinal], reinterpret_cast<void *>(args), ordinal, library.contract, unix_check_range)) : 0xc000000du;
			if (trace_unix) fprintf(stderr, "unix leave %u status=%llx\n", ordinal, (unsigned long long)status);
		}
		// BeginSimulation clears the suspend doorbell on reentry. A hot unix
		// polling loop must not discard a scheduler interrupt on every call.
		if (__atomic_load_n(wow64_module ? wow64_unix_doorbell : RavenNT::active_thread->cpu.doorbell, __ATOMIC_ACQUIRE)) {
			memcpy(static_cast<uint8_t *>(packet) + (wow64_module ? 0xb0 : 0x78), &status, wow64_module ? 4 : 8);
			last_stop = {raven::NativeStop::timeout, 0, wow64_module ? uint32_t(packet_word(packet, 0xb8)) : packet_word(packet, 0xf8), {}};
			siglongjmp(stop_frame, 1);
		}
		return status;
	}
	auto rip = wow64_module ? *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(packet) + 0xb8) : packet_word(packet, 0xf8);
	last_stop = {number == completion ? raven::NativeStop::returned : raven::NativeStop::syscall,
		number == completion ? (wow64_module ? *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(packet) + 0xa8) : packet_word(packet, 0xc8)) : number, rip, {}};
	siglongjmp(stop_frame, 1);
}
int32_t process_exception(void *opaque, void *packet, void *native_context)
{
	RavenFault::alignment_recovery = nullptr;
	auto &record = *static_cast<RavenFault::Record *>(opaque);
	if (!wow64_module && std::getenv("RAVEN_FEX_SNAPSHOT_TRACE")) {
		struct Snapshot { uint64_t rip, r9, r10, r11, pc; };
		static thread_local Snapshot ring[32]{};
		static thread_local unsigned index=0;
		auto *nc=static_cast<RavenFault::NativeContext *>(native_context);
		ring[index++ % 32]={packet_word(packet,0xf8),packet_word(packet,0xc0),packet_word(packet,0xc8),packet_word(packet,0xd0),nc->pc};
		if (record.code == 0xc0000005) {
			for (unsigned n=0;n<32;++n) {
				auto &v=ring[(index+n)%32];
				fprintf(stderr,"SNAP rip=%llx r9=%llx r10=%llx r11=%llx pc=%llx\n",v.rip,v.r9,v.r10,v.r11,v.pc);
			}
			for(unsigned n=0;n<31;++n) fprintf(stderr,"HOST x%u=%llx\n",n,nc->x[n]);
			char path[128]; snprintf(path,sizeof path,"/tmp/raven-chrome-fault-%d-%llx.bin",getpid(),nc->pc);
			std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char *>(nc->pc & ~uint64_t(4095)),4096);
		}
	}

	last_stop = {record.code == 0x102 ? raven::NativeStop::timeout : raven::NativeStop::exception, record.code, (wow64_module ? *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(packet) + 0xb8) : packet_word(packet, 0xf8)), record};
	siglongjmp(stop_frame, 1);
}
uint32_t guest_protection(int protection)
{
	if (protection & PROT_EXEC) return protection & PROT_WRITE ? 0x40 : 0x20;
	return protection & PROT_WRITE ? 4 : protection & PROT_READ ? 2 : 1;
}
void notify_memory(const char *name, uint64_t *args, size_t after, const std::function<void()> &change = {})
{
	static fex_perf::Counter perf{"notify_memory"};
	fex_perf::Timer timer(perf);
	const std::string export_name = wow64_module ? std::string("BTCpu") + name : name;
	name = export_name.c_str();
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

namespace {
// Controller-only measurements; disabled runs do not read the clock.
struct FexCallTimings {
    bool enabled = std::getenv("RAVEN_FEX_PROFILE_CALLS") != nullptr;
    uint64_t count[3]{}, ns[3]{};
    ~FexCallTimings() {
        if (!enabled) return;
        const char *names[]{"resume", "context", "arguments"};
        for (unsigned i=0; i<3; ++i)
            fprintf(stderr, "FEX calls: %s count=%llu ns=%llu\n", names[i], (unsigned long long)count[i], (unsigned long long)ns[i]);
    }
} fex_call_timings;
struct FexCallTimer {
    unsigned index;
    std::chrono::steady_clock::time_point start;
    FexCallTimer(unsigned i) : index(i), start(fex_call_timings.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~FexCallTimer() {
        if (!fex_call_timings.enabled) return;
        ++fex_call_timings.count[index];
        fex_call_timings.ns[index] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    }
};
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
	uint32_t *interrupt = nullptr;
	int descriptor;
	explicit FexWorker(int fd) : descriptor(fd) {
		host = std::thread([this] {
			AlternateSignalStack alternate;
			try {
				environment = std::make_unique<RavenNT::ThreadEnvironment>(*process_environment);
				RavenNT::active_thread = environment.get();
				mapped_image->install_tls(*environment);
				uint64_t args[8]{};
				if (!wow64_module && raven_ec_call(mapped_image->export_address("ThreadInit"), args, environment->teb.data()))
					throw std::runtime_error("FEX worker ThreadInit failed");
				if (wow64_module) {
					raven_ec_call(mapped_image->export_address("BTCpuThreadInit"), args, environment->teb.data());
					initialize_wow64_packet(*environment);
					// FEX InternalThreadState::CurrentFrame is const and points to BaseFrameState.
					interrupt = reinterpret_cast<uint32_t *>(raven_ec_call(mapped_image->export_address("RavenWow64Doorbell"), args, environment->teb.data()));
					if (!interrupt) throw std::runtime_error("FEX WoW64 needs a suspend doorbell; rebuild the module");
					wow64_unix_doorbell = interrupt;
				}
				std::unique_lock lock(mutex);
				ready = true;
				wake.notify_all();
				for (;;) {
					wake.wait(lock, [this] { return requested || quit; });
					if (quit) break;
					requested = false;
					wow64_teb = teb;
					guest_teb = !wow64_module && teb ? reinterpret_cast<void *>(teb) : environment->teb.data();
					RavenFault::execution_teb = guest_teb;
					lock.unlock();
					FaultDispatchState dispatch_state;
					auto previous = NativeJit::protect(1);
					if (!sigsetjmp(stop_frame, 1)) {
						raven_ec_call(mapped_image->export_address(wow64_module ? "RavenWow64Run" : "BeginSimulation"), args, guest_teb);
						std::abort();
					}
					asm volatile("mov x18,xzr");
                    dispatch_state.restore();
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
				if (wow64_module) raven_ec_call(mapped_image->export_address("BTCpuThreadTerm"), args, environment->teb.data());
				else if (raven_ec_call(mapped_image->export_address("ThreadTerm"), args, environment->teb.data()))
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
			if (wow64_module) __atomic_store_n(interrupt, 1u, __ATOMIC_RELEASE);
			else __atomic_store_n(environment->cpu.doorbell, 1u, __ATOMIC_RELEASE);
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
	NativeMode mode = NativeMode::x64;
	std::unique_ptr<FexGuestWindow> window;
	uint64_t host(uint64_t address, size_t size = 1) const { return window ? uintptr_t(window->host(address, size)) : address; }
	uint64_t guest(const void *address) const { return uintptr_t(address) - (window ? window->base() : 0); }
	uint32_t *interrupt = nullptr;
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
	// A Windows-page-aligned shared view can begin inside a Darwin page.
	std::map<uint64_t, uint64_t> shared_view_bases;
	// Reserved pages have implicit PROT_NONE; store only explicit page overrides.
	std::map<uint64_t, int> pages;
	bool mapped_range(uint64_t address, uint64_t size) const {
		if (!size || address > UINT64_MAX - size) return false;
		const uint64_t end = address + size;
		while (address < end) {
			auto region = regions.upper_bound(address);
			if (region != regions.begin()) {
				--region;
				if (address - region->first < region->second.size) {
					address = std::min(end, region->first + region->second.size);
					continue;
				}
			}
			if (!pages.contains(address & ~uint64_t(4095))) return false;
			address = std::min(end, (address & ~uint64_t(4095)) + 4096);
		}
		return true;
	}
	std::map<uint64_t, uint32_t> traps;
	std::map<uint64_t, size_t> image_reservations;
	std::vector<LoadedImage *> images;
	NativeThreadState thread;
	NativeContext initial;
	std::function<void()> resume_hook;
	uint64_t teb = 0;
	uint64_t user_limit = 0x7fff0000ULL;
	uint64_t stack_top = 0;
	std::function<void()> unix_pump;
	std::chrono::milliseconds budget{5000}, slice{0}, sample_period{0};
	std::function<void(uint64_t)> sampler;
	std::chrono::steady_clock::time_point slice_started = std::chrono::steady_clock::now();
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
		if (!window && stack != MAP_FAILED) munmap(stack, stack_size);
		if (!window && gate != MAP_FAILED) munmap(gate, 16384);
		if (!window) for (auto [base, region] : regions) release_guest_range(base, region.size);
		for (auto [base, size] : image_reservations) release_guest_range(base, size);
	}
	void require_owner() const
	{
		if (!pthread_equal(owner, pthread_self()))
			throw std::runtime_error("FEX process used from another host thread");
	}
};
FexProcess::FexProcess(std::span<LoadedImage *const> images, const std::string &root, uint64_t stack_reserve, NativeMode mode)
	: state(std::make_unique<State>())
{
	if (module_claimed)
		throw std::runtime_error("FEX module already claimed; a second FexProcess in this host is unsupported");
	state->mode = mode;
	wow64_module = mode == NativeMode::x86;
	unix_read_memory = [this](uint64_t address, size_t size) { return read_memory(address, size); };
	if (wow64_module) {
		// The executable, not its DLLs, selects the WoW64 user range.
		for (auto *image : images) {
			const Bytes data{image->data().data(), image->data().size()};
			const auto pe = data.get<uint32_t>(0x3c);
			const auto flags = data.get<uint16_t>(pe + 22);
			if (!(flags & 0x2000)) { state->user_limit = wow64_user_limit(flags); break; }
		}
		state->window = std::make_unique<FexGuestWindow>();
		wow64_guest_base = state->window->base();
	}
	state->stack_size = (std::clamp<uint64_t>(stack_reserve, 256 * 1024, 64 * 1024 * 1024) + 16383) & ~uint64_t(16383);
	if (root.empty())
		throw std::runtime_error("FEX requires RAVEN_FEX_MODULE_ROOT");
	// Reserve initial preferred/fallback bases before FEX/dyld initialization
	// can claim them. map_image replaces only these owned reservations.
	if (!wow64_module) for (auto *image : images) {
		const auto n = (image->data().size() + 16383) & ~size_t(16383);
		mach_vm_address_t base = image->base();
		auto reserved = pending_images.ranges.find(base);
		if (reserved != pending_images.ranges.end() && reserved->second == n)
			pending_images.ranges.erase(reserved);
		else if (!reserve_guest_range(base, n))
			throw std::runtime_error("FEX guest image reservation conflict at " + std::to_string(image->base()));
		state->image_reservations.emplace(base, n);
	}
	module_claimed = true;
	std::string path = root + (wow64_module ? "/xtajit.dll" : "/xtajit64.dll");
	char program[] = "raven-fex";
	char *argv[]{program, path.data()};
	if (map_module(2, argv, resolve, inspect_image, initialize_process, module_mapping))
		throw std::runtime_error("FEX module initialization failed");
	if (!wow64_module && std::getenv("RAVEN_FEX_PROFILE_COMPILE")) {
        uint64_t args[8]{1, 0};
        raven_ec_call(mapped_image->export_address("RavenCompileProfile"), args, thread_environment->teb.data());
    }
	// Guest x86 code is data to the arm64 host. Keep it non-executable; only FEX's
	// separate native code cache uses MAP_JIT. Never replace an existing host mapping.
	for (auto *image : images) map_image(*image);
	state->stack = wow64_module ? reinterpret_cast<void *>(state->host(map_memory(state->stack_size, PROT_READ | PROT_WRITE, 0))) : mmap(nullptr, state->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	state->gate = wow64_module ? reinterpret_cast<void *>(state->host(map_memory(16384, PROT_READ | PROT_WRITE | PROT_EXEC, 0))) : mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (state->stack == MAP_FAILED || state->gate == MAP_FAILED)
		throw std::runtime_error("FEX guest stack/gate allocation failed");
	// mov r10,rax; mov eax,completion; syscall; ud2
	uint8_t gate[]{0x49, 0x89, 0xc2, 0xb8, 0xfe, 0xff, 0xff, 0x7f, 0x0f, 0x05, 0x0f, 0x0b};
	if (wow64_module) {
		const uint8_t gate32[]{0x89, 0xc2, 0xb8, 0xfe, 0xff, 0xff, 0x7f, 0xcd, 0x2e, 0x0f, 0x0b};
		memcpy(state->gate, gate32, sizeof(gate32));
		// Wine PE32 stdcall dispatcher: uint64 handle, uint32 ordinal, uint32 args.
		const uint8_t unix_gate32[]{0xb8,0xfd,0xff,0xff,0x7f,0xcd,0x2e,0xc2,0x10,0x00};
		memcpy(static_cast<uint8_t *>(state->gate) + 64, unix_gate32, sizeof(unix_gate32));
	} else {
		memcpy(state->gate, gate, sizeof(gate));
		// Win64 unix dispatcher: mov r10,rcx; mov eax,unix_service; syscall; ret.
		const uint8_t unix_gate[]{0x49,0x89,0xca,0xb8,0xfd,0xff,0xff,0x7f,0x0f,0x05,0xc3};
		memcpy(static_cast<uint8_t *>(state->gate) + 64, unix_gate, sizeof(unix_gate));
	}
	uint64_t args[8]{uintptr_t(state->gate), 16384, 0x3000, 0x40, 1, 0};
	if (wow64_module) protect_memory(state->guest(state->gate), 16384, PROT_READ | PROT_EXEC);
	else notify_memory("NotifyMemoryAlloc", args, 4);
	state->stack_top = state->guest(state->stack) + state->stack_size;
	state->thread.stack_top = state->stack_top;
	for (uint64_t at = state->guest(state->stack); at < state->stack_top; at += 4096) state->pages[at] = PROT_READ | PROT_WRITE;
	state->pages[state->guest(state->gate)] = PROT_READ | PROT_EXEC;
	if (state->parallel) {
		state->execution = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (state->execution < 0) throw std::runtime_error("FEX execution eventfd failed");
		state->selected = state->make_worker();
		state->thread.task = state->selected;
	}
	state->initial = context();
	state->faults = std::make_unique<RavenFault::Scope>(mapped_image->export_address("RavenHandleFault"));
}
FexProcess::~FexProcess()
{
	// Destruction is noexcept: fail fast rather than finalize another pthread's
	// allocator/TLS or silently continue after failed module teardown.
	if (!pthread_equal(state->owner, pthread_self())) {
		std::fputs("FEX destruction requires the process owner thread\n", stderr);
		std::terminate();
	}
	if (!wow64_module && std::getenv("RAVEN_FEX_PROFILE_COMPILE")) {
        uint64_t values[2]{};
        uint64_t args[8]{1, uintptr_t(values)};
        raven_ec_call(mapped_image->export_address("RavenCompileProfile"), args, thread_environment->teb.data());
        fprintf(stderr, "FEX compile: blocks=%llu ns=%llu\n", (unsigned long long)values[0], (unsigned long long)values[1]);
        for (uint64_t mode : {2, 3}) for (uint64_t index = 0;; ++index) {
            uint64_t record[4]{index};
            uint64_t query[8]{mode, uintptr_t(record)};
            raven_ec_call(mapped_image->export_address("RavenCompileProfile"), query, thread_environment->teb.data());
            if (mode == 2 ? !record[1] : !record[3]) break;
            if (mode == 2)
                fprintf(stderr, "Raven profile pid=%d Raven translations page=%llx unique=%llu repeated=%llu recompiled=%llu\n", getpid(), (unsigned long long)record[0], (unsigned long long)record[1], (unsigned long long)record[2], (unsigned long long)record[3]);
            else
                fprintf(stderr, "Raven profile pid=%d Raven invalidation reason=%llu start=%llx size=%llu count=%llu\n", getpid(), (unsigned long long)record[0], (unsigned long long)record[1], (unsigned long long)record[2], (unsigned long long)record[3]);
        }
    }
	// Resident workers terminate their FEX thread before releasing private TLS.
	state->workers.clear();
	if (thread_environment) {
		uint64_t args[8]{uint64_t(-2), 0};
		RavenFault::execution_teb = nullptr;
		const auto status = raven_ec_call(mapped_image->export_address(wow64_module ? "BTCpuThreadTerm" : "ThreadTerm"),
			args, thread_environment->teb.data());
		// BTCpuThreadTerm is void; only the x64 API returns NTSTATUS.
		if (!wow64_module && status) {
			std::fputs("FEX owner ThreadTerm failed\n", stderr);
			std::terminate();
		}
	}
	state.reset();
	RavenNT::active_thread = nullptr;
	thread_environment.reset();
}
NativeResult FexProcess::call(uint64_t address, std::span<const uint64_t> arguments)
{
	state->require_owner();
	if (state->active || arguments.size() > 128)
		throw std::runtime_error("FEX entry requires an idle frame and at most 128 arguments");
	if (wow64_module) {
		auto saved = context();
		auto &r = saved.registers;
		r.rip = uint32_t(address);
		r.rsp = ((state->thread.stack_top - arguments.size() * 4) & ~uint64_t(15)) - 4;
		std::vector<uint8_t> stack((arguments.size() + 1) * 4);
		const uint32_t gate = uint32_t(return_gate()); memcpy(stack.data(), &gate, 4);
		for (size_t i = 0; i < arguments.size(); ++i) { const uint32_t word = uint32_t(arguments[i]); memcpy(stack.data() + 4 + i * 4, &word, 4); }
		write_memory(r.rsp, stack);
		set_context(saved);
		state->active = state->thread.active_call = true;
		state->thread.awaiting_api = false;
		return resume(state->budget);
	}
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
	state->thread.awaiting_api = state->thread.awaiting_syscall = false;
	return resume(state->budget);
}
NativeResult FexProcess::resume()
{
	FexCallTimer timing{0};
	state->require_owner();
	if (!state->active || state->thread.awaiting_api)
		throw std::runtime_error("FEX has no resumable call or awaits an API result");
	state->thread.awaiting_syscall = state->thread.debug_print_fault = false;
	if (state->resume_hook) state->resume_hook();
	if (!unix_libraries.empty() && state->unix_pump) state->unix_pump();
	if (state->parallel) {
		auto *worker = state->workers.at(state->selected).get();
		const auto deadline = std::chrono::steady_clock::now() + state->budget;
		{
			std::lock_guard lock(worker->mutex);
			worker->teb = state->teb;
			if (wow64_module) __atomic_store_n(worker->interrupt, 0u, __ATOMIC_RELEASE);
			else __atomic_store_n(worker->environment->cpu.doorbell, 0u, __ATOMIC_RELEASE);
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
		if (wow64_module) {
			uint64_t query[8]{};
			state->interrupt = reinterpret_cast<uint32_t *>(raven_ec_call(mapped_image->export_address("RavenWow64Doorbell"), query, state->environment()->teb.data()));
			if (!state->interrupt) throw std::runtime_error("FEX WoW64 needs a suspend doorbell; rebuild the module");
			wow64_unix_doorbell = state->interrupt;
		}
		std::jthread timer([&](std::stop_token stop) {
			std::unique_lock lock(timer_mutex);
			timer_wake.wait_for(lock, stop, state->budget, [] { return false; });
			if (!stop.stop_requested()) {
				if (wow64_module) __atomic_store_n(state->interrupt, 1u, __ATOMIC_RELEASE);
				else __atomic_store_n(doorbell, 1u, __ATOMIC_RELEASE);
			}
		});
		uint64_t args[8]{};
		FaultDispatchState dispatch_state;
		auto previous = NativeJit::protect(1);
		if (!sigsetjmp(stop_frame, 1)) {
			raven_ec_call(mapped_image->export_address(wow64_module ? "RavenWow64Run" : "BeginSimulation"), args, wow64_module ? state->environment()->teb.data() : guest_teb ? guest_teb : state->environment()->teb.data());
			std::abort();
		}
		timer.request_stop();
		timer_wake.notify_all();
		timer.join();
		if (wow64_module) __atomic_store_n(state->interrupt, 0u, __ATOMIC_RELEASE);
		else __atomic_store_n(doorbell, 0u, __ATOMIC_RELEASE);
		asm volatile("mov x18,xzr");
        dispatch_state.restore();
		NativeJit::protect(previous);
	}
	static const bool trace_stops = std::getenv("RAVEN_FEX_TRACE") != nullptr;
	if (trace_stops)
		std::cerr << "FEX stop rip=" << std::hex << last_stop.rip << " address=" << last_stop.exception.information[1] << std::dec << "\n";
	state->thread.awaiting_syscall = last_stop.kind == NativeStop::syscall;
	if (state->thread.awaiting_syscall) state->thread.context.registers.orig_rax = last_stop.value;
	if (last_stop.kind == NativeStop::returned)
		state->active = false;
	state->thread.awaiting_syscall = last_stop.kind == NativeStop::syscall;
	state->thread.context.registers.orig_rax = last_stop.value;
	NativeResult result{last_stop.kind, last_stop.value, last_stop.rip,
		last_stop.kind == NativeStop::exception ? SIGSEGV : 0};
	state->thread.debug_print_fault = false;
	if (last_stop.kind == NativeStop::exception) {
		auto &record = last_stop.exception;
		// INT 2D debug-print handling is restricted to the canonical provider body.
		if (!state->traps.empty() && result.value == 0x80000003 && record.flags == 0 &&
			record.count == 1 && record.information[0] == 1 && context(false).registers.rax == 1 &&
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
NativeContext FexProcess::context(bool full) const
{
	static fex_perf::Counter perf{"context"};
	fex_perf::Timer timer(perf);
	FexCallTimer timing{1};
	state->require_owner();
	if (!stopped()) throw std::runtime_error("FEX context requires a stopped worker");
	NativeContext result{};
	auto &r = result.registers;
	r.orig_rax = state->thread.awaiting_syscall ? state->thread.context.registers.orig_rax : uint64_t(-1);
	auto *p = state->environment()->context.data();
	if (wow64_module) {
		unsigned long long *registers[]{&r.rdi, &r.rsi, &r.rbx, &r.rdx, &r.rcx, &r.rax, &r.rbp, &r.rip};
		for (size_t i = 0; i < 8; ++i) *registers[i] = *reinterpret_cast<uint32_t *>(p + 0x9c + i * 4);
		r.rsp = *reinterpret_cast<uint32_t *>(p + 0xc4); r.eflags = *reinterpret_cast<uint32_t *>(p + 0xc0);
		r.fs_base = state->teb;
		r.gs = *reinterpret_cast<uint32_t *>(p+0x8c); r.fs = *reinterpret_cast<uint32_t *>(p+0x90);
		r.es = *reinterpret_cast<uint32_t *>(p+0x94); r.ds = *reinterpret_cast<uint32_t *>(p+0x98);
		r.cs = *reinterpret_cast<uint32_t *>(p+0xbc); r.ss = *reinterpret_cast<uint32_t *>(p+0xc8);
		if (full) result.xstate.assign(p + 0xcc, p + 0x2cc);
		return result;
	}
	unsigned long long *registers[]{&r.rax, &r.rcx, &r.rdx, &r.rbx, &r.rsp, &r.rbp, &r.rsi, &r.rdi,
		&r.r8, &r.r9, &r.r10, &r.r11, &r.r12, &r.r13, &r.r14, &r.r15, &r.rip};
	for (size_t i = 0; i < 17; ++i)
		*registers[i] = packet_word(p, 0x78 + i * 8);
	r.orig_rax = state->thread.awaiting_syscall ? state->thread.context.registers.orig_rax : uint64_t(-1);
	r.gs_base = state->teb;
	r.eflags = *reinterpret_cast<uint32_t *>(p + 0x44);
	r.cs = *reinterpret_cast<uint16_t *>(p+0x38); r.ds = *reinterpret_cast<uint16_t *>(p+0x3a);
	r.es = *reinterpret_cast<uint16_t *>(p+0x3c); r.fs = *reinterpret_cast<uint16_t *>(p+0x3e);
	r.gs = *reinterpret_cast<uint16_t *>(p+0x40); r.ss = *reinterpret_cast<uint16_t *>(p+0x42);
	if (!full) return result;
	result.xstate.assign(p + 0x100, p + 0x300);
	// The public legacy CONTEXT packet has no YMM upper halves. Keep them in
	// the internal XSAVE-shaped snapshot so cooperative threads cannot share
	// the translator's otherwise persistent high-vector state.
	result.xstate.resize(832);
	const uint64_t xstate_bv = 7;
	memcpy(result.xstate.data() + 512, &xstate_bv, 8);
	uint64_t vector_args[8]{uintptr_t(result.xstate.data() + 576)};
	raven_ec_call(mapped_image->export_address("RavenGetYmmHigh"), vector_args, state->environment()->teb.data());
	return result;
}
void FexProcess::set_context(const NativeContext &value)
{
	state->require_owner();
	if (!stopped()) throw std::runtime_error("FEX context update requires a stopped worker");
	const auto &r = value.registers;
    if (!value.xstate.empty() && (wow64_module ? value.xstate.size()!=512 :
        value.xstate.size()!=512 && value.xstate.size()!=832))
        throw std::runtime_error("invalid FEX floating-point packet");
	if (wow64_module) {
		auto *p = state->environment()->context.data();
		const uint32_t regs[]{uint32_t(r.rdi), uint32_t(r.rsi), uint32_t(r.rbx), uint32_t(r.rdx), uint32_t(r.rcx), uint32_t(r.rax), uint32_t(r.rbp), uint32_t(r.rip)};
		memcpy(p + 0x9c, regs, sizeof(regs));
		const uint32_t sp = uint32_t(r.rsp), flags = uint32_t(r.eflags);
		memcpy(p + 0xc4, &sp, 4); memcpy(p + 0xc0, &flags, 4);
        const uint32_t segments[]{uint32_t(r.gs),uint32_t(r.fs),uint32_t(r.es),uint32_t(r.ds)};
        memcpy(p+0x8c,segments,sizeof segments);
        const uint32_t cs=r.cs, ss=r.ss; memcpy(p+0xbc,&cs,4); memcpy(p+0xc8,&ss,4);
		if (!value.xstate.empty()) {
			if (value.xstate.size() != 512) throw std::runtime_error("WoW64 requires a 512-byte FP packet");
			memcpy(p + 0xcc, value.xstate.data(), 512);
		}
		state->teb = wow64_teb = r.fs_base;
		return;
	}
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
	if (!value.xstate.empty() && value.xstate.size() != 512 && value.xstate.size() != 832)
		throw std::runtime_error("FEX requires a legacy or YMM floating-point packet");
	const uint64_t registers[]{r.rax, r.rcx, r.rdx, r.rbx, r.rsp, r.rbp, r.rsi, r.rdi,
		r.r8, r.r9, r.r10, r.r11, r.r12, r.r13, r.r14, r.r15, r.rip};
	auto *p = state->environment()->context.data();
	memcpy(p + 0x78, registers, sizeof(registers));
	uint32_t flags = uint32_t(r.eflags);
	memcpy(p + 0x44, &flags, 4);
    const uint16_t segments[]{uint16_t(r.cs),uint16_t(r.ds),uint16_t(r.es),uint16_t(r.fs),uint16_t(r.gs),uint16_t(r.ss)};
    memcpy(p+0x38,segments,sizeof segments);
	if (value.xstate.size() == 832) {
		uint64_t vector_args[8]{uintptr_t(value.xstate.data() + 576)};
		raven_ec_call(mapped_image->export_address("RavenSetYmmHigh"), vector_args, state->environment()->teb.data());
	}
	if (!value.xstate.empty()) { memcpy(p + 0x100, value.xstate.data(), 512); memcpy(p + 0x34, value.xstate.data()+24, 4); }
}
NativeResult run_fex_entry(const PEImage &image, LoadedImage &loaded, const std::string &root)
{
	if ((image.characteristics & 0x2000) || !image.imports().empty() ||
		image.directories[9].size || image.directories[13].size || image.directories[14].size)
		throw std::runtime_error("FEX entry requires PE32/PE64 without imports, TLS, delayed imports or CLR");
	LoadedImage *images[]{&loaded};
	FexProcess process(images, root, image.stack_reserve, image.is_64 ? NativeMode::x64 : NativeMode::x86);
	return process.call(loaded.base() + image.entry, {});
}
}

namespace raven {
void select_fex_execution(bool enabled) { selected_fex = enabled; }
bool fex_execution_selected() { return selected_fex; }
bool fex_address_available(uint64_t address, uint64_t size)
{
	if (!size || address < 0x100000000ULL || address > UINT64_MAX - size) return false;
	if (image_pool.contains(address, size)) return image_pool.available(address, size);
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
void fex_register_image_reservation(uint64_t address, uint64_t size)
{
	if (image_pool.size || !address || !size || mprotect(reinterpret_cast<void *>(address), size, PROT_NONE))
		throw std::runtime_error("FEX cannot initialize Mach-O image reservation");
	image_pool.base = address;
	image_pool.size = size;
}
bool fex_image_pool_claimed(uint64_t address) { return image_pool.claims.contains(address); }
bool fex_reserve_image_address(uint64_t address, uint64_t size)
{
	// Loader preflight must claim the range immediately: even host allocation
	// during PE preparation can otherwise occupy a free preferred base.
	const auto n = (size + 16383) & ~uint64_t(16383);
	if (auto it = pending_images.ranges.find(address); it != pending_images.ranges.end())
		return it->second == n;
	mach_vm_address_t base = address;
	if (!reserve_guest_range(base, n)) {
		if (std::getenv("RAVEN_FEX_TRACE_MEMORY")) {
			mach_vm_address_t at = address;
			mach_vm_size_t length = 0;
			vm_region_extended_info_data_t info{};
			mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
			mach_port_t object = MACH_PORT_NULL;
			const auto status = mach_vm_region(mach_task_self(), &at, &length, VM_REGION_EXTENDED_INFO,
				reinterpret_cast<vm_region_info_t>(&info), &count, &object);
			if (object) mach_port_deallocate(mach_task_self(), object);
			char path[PROC_PIDPATHINFO_MAXSIZE]{};
			proc_regionfilename(getpid(), at, path, sizeof(path));
			std::cerr << "FEX collision requested=" << std::hex << address << "+" << n
				<< " region=" << at << "+" << length << std::dec << " tag=" << info.user_tag
				<< " protection=" << info.protection << " status=" << status << " path=" << path << "\n";
		}
		return false;
	}
	pending_images.ranges.emplace(address, n);
	return true;
}
bool FexProcess::memory_available(uint64_t address, size_t size) const { if (state->window) {
		if (!size || address < 65536 || address >= state->user_limit || size > state->user_limit - address) return false;
		for (auto [base, region] : state->regions) if (address < base + region.size && base < address + size) return false;
		return true;
	} return fex_address_available(address, size); }
std::vector<uint8_t> FexProcess::read_memory(uint64_t address, size_t size) const
{
	static fex_perf::Counter perf{"read_memory"};
	fex_perf::Timer timer(perf);
	state->require_owner();
	if (address > UINT64_MAX - size) throw std::runtime_error("FEX read range overflow");
	for (auto at = address & ~uint64_t(4095); at < address + size; at += 4096)
		if (!state->pages.contains(at) || !(state->pages.at(at) & PROT_READ)) {
			if (!state->window && read_native_buffer) {
				std::vector<uint8_t> bytes(size);
				if (read_native_buffer(address, bytes.data(), size)) return bytes;
			}
			throw std::runtime_error("FEX read exceeds readable guest memory at " + std::to_string(address));
		}
	auto *p = !state->window && address >= 0x7ffe0000 && address + size <= 0x7ffe1000 ? user_shared_data.data() + address - 0x7ffe0000 : reinterpret_cast<const uint8_t *>(state->host(address, size));
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
	static fex_perf::Counter perf{"write_memory"};
	fex_perf::Timer timer(perf);
	validate_write_memory(address, bytes.size());
	auto *p = !state->window && address >= 0x7ffe0000 && address + bytes.size() <= 0x7ffe1000 ? user_shared_data.data() + address - 0x7ffe0000 : reinterpret_cast<uint8_t *>(state->host(address, bytes.size()));
	memcpy(p, bytes.data(), bytes.size());
	uint64_t args[8]{state->host(address, bytes.size()), bytes.size()};
	raven_ec_call(mapped_image->export_address(wow64_module ? "BTCpuNotifyMemoryDirty" : "BTCpu64NotifyMemoryDirty"), args, state->environment()->teb.data());
}
uint64_t FexProcess::map_memory(size_t size, int protection, uint64_t address)
{
	state->require_owner();
	if (!size || size > SIZE_MAX - 65536) throw std::runtime_error("FEX invalid allocation size");
	if (!state->window && address == 0x7ffe0000 && size == 4096) {
		memset(user_shared_data.data(), 0, user_shared_data.size());
		state->pages[address] = protection;
		return address;
	}
	const auto n = (size + 16383) & ~size_t(16383);
	if (state->window) {
		if (!address) for (address = 0x10000; address < state->user_limit && !memory_available(address, n); address += 65536) {}
		if (!memory_available(address, n) || address % 16384) throw std::runtime_error("WoW64 guest allocation unavailable");
		if (mprotect(reinterpret_cast<void *>(state->host(address, n)), n, PROT_READ | PROT_WRITE)) throw std::runtime_error("WoW64 guest commit failed");
	} else if (address) {
		mach_vm_address_t base = address;
		if (address % 16384 || !reserve_guest_range(base, n))
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
	unix_ranges.set(state->host(address, n), n, PROT_NONE);
	uint64_t args[8]{state->host(address, n), n, 0x3000, guest_protection(protection), 0, 0};
	notify_memory("NotifyMemoryAlloc", args, 4);
	if (protection != PROT_NONE)
		for (size_t at = 0; at < n; at += 4096) state->pages[address + at] = PROT_READ | PROT_WRITE;
	protect_memory(address, n, protection);
	return address;
}
void FexProcess::flush_instruction_cache(uint64_t address, size_t size)
{
	const auto notify = [&](uint64_t start, size_t length) {
		uint64_t args[8]{start ? state->host(start, length) : 0, length};
		raven_ec_call(mapped_image->export_address(wow64_module ? "BTCpuFlushInstructionCache2" : "BTCpu64FlushInstructionCache"), args, state->environment()->teb.data());
	};
	if (!address) { notify(0, 0); return; }
	if (!size) return;
	const uint64_t end = size > UINT64_MAX - address ? UINT64_MAX : address + size;
	// Ignore holes and out-of-window addresses rather than translating an
	// arbitrary API range to a host pointer (which can throw in WoW64).
	auto page = state->pages.lower_bound(address & ~uint64_t(4095));
	while (page != state->pages.end() && page->first < end) {
		const auto start = std::max(address, page->first);
		auto stop = page->first + 4096;
		while (++page != state->pages.end() && page->first == stop && stop < end) stop += 4096;
		stop = std::min(end, stop);
		if (start < stop && state->mapped_range(start, stop - start)) notify(start, stop - start);
	}
}
void FexProcess::protect_memory(uint64_t address, size_t size, int protection)
{
	static fex_perf::Counter perf{"protect_memory"};
	fex_perf::Timer timer(perf);
	if (!size || address % 4096 || size % 4096 || address > UINT64_MAX - size)
		throw std::runtime_error("FEX invalid protection range");
	if (!state->mapped_range(address, size)) throw std::runtime_error("FEX protection range is unmapped");
	uint64_t args[8]{state->host(address, size), size, guest_protection(protection)};
	notify_memory("NotifyMemoryProtect", args, 3, [&] {
		if (protection == PROT_NONE && !(address % 16384) && !(size % 16384)) {
			if (mprotect(reinterpret_cast<void *>(state->host(address, size)), size, PROT_NONE))
				throw std::runtime_error("FEX host protection failed");
			state->pages.erase(state->pages.lower_bound(address), state->pages.lower_bound(address + size));
			unix_ranges.set(state->host(address, size), size, protection);
			return;
		}
		for (uint64_t at = address; at < address + size; at += 4096) state->pages[at] = protection;
		unix_ranges.set(state->host(address, size), size, protection);
		if (!state->window && address == 0x7ffe0000 && size == 4096) return;
		// Tier 0: union the four logical protections of each real 16K page. Guest
		// executable bytes remain host data; MAP_JIT owns native executable code.
		for (auto page = address & ~uint64_t(16383); page < address + size; page += 16384) {
			int combined = 0;
			for (uint64_t at = page; at < page + 16384; at += 4096) {
				const auto found = state->pages.find(at);
				if (found != state->pages.end()) combined |= found->second;
			}
			combined = (combined & (PROT_READ | PROT_WRITE)) | (combined & PROT_EXEC ? PROT_READ : 0);
			if (mprotect(reinterpret_cast<void *>(state->host(page, 16384)), 16384, combined)) throw std::runtime_error("FEX host protection failed");
		}
	});
}
void FexProcess::protect_image(uint64_t address, size_t size, int protection) { protect_memory(address, size, protection); }
bool FexProcess::has_protection(uint64_t address, size_t size, int protection) const
{
	if (!size || address > UINT64_MAX - size) return false;
	for (auto at = address & ~uint64_t(4095); at < address + size; at += 4096) {
		auto found = state->pages.find(at);
		if (found == state->pages.end() ? protection != PROT_NONE || !state->mapped_range(at, 4096) : found->second != protection) return false;
	}
	return true;
}
void FexProcess::unmap_memory(uint64_t address)
{
	const auto view = state->shared_view_bases.find(address);
	if (view != state->shared_view_bases.end()) address = view->second;
	auto found = state->regions.find(address);
	if (found == state->regions.end()) throw std::runtime_error("FEX allocation not found");
	const auto n = found->second.size;
	unix_ranges.set(state->host(address, n), n, PROT_NONE);
	uint64_t args[8]{state->host(address, n), n, 0x8000};
	notify_memory("NotifyMemoryFree", args, 3, [&] {
		if (state->window) {
			void *p = mmap(reinterpret_cast<void *>(state->host(address, n)), n, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
			if (p == MAP_FAILED) throw std::runtime_error("WoW64 cannot restore reservation");
		} else release_guest_range(address, n);
	});
	state->pages.erase(state->pages.lower_bound(address), state->pages.lower_bound(address + n));
	state->regions.erase(found);
	if (view != state->shared_view_bases.end()) state->shared_view_bases.erase(view);
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
	if ((state->window && !memory_available(address, n)) || (!state->window && !state->image_reservations.contains(address) && !reserve_guest_range(base, n)))
		throw std::runtime_error("FEX guest image address conflict at " + std::to_string(address));
	auto *p = mmap(reinterpret_cast<void *>(state->host(address, n)), n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, image.descriptor(), 0);
	if (p == MAP_FAILED) {
		state->image_reservations.erase(address);
		if (state->window)
			(void)mmap(reinterpret_cast<void *>(state->host(address, n)), n, PROT_NONE,
				MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
		else
			release_guest_range(address, n);
		throw std::runtime_error("FEX shared image mapping failed");
	}
	state->image_reservations.erase(address);
	state->regions.emplace(address, State::Region{n, true});
	unix_ranges.set(state->host(address, n), n, PROT_NONE);
	state->images.push_back(&image);
	if (std::getenv("RAVEN_FEX_PROFILE_COMPILE")) {
		// Read only bounded PE metadata from the loader's image view.
		auto bytes = image.data();
		auto word = [&](size_t at) { uint32_t v = 0; if (at <= bytes.size() && bytes.size() - at >= 4) memcpy(&v, bytes.data() + at, 4); return v; };
		auto pe = word(0x3c);
		auto exp = word(size_t(pe) + 24 + (wow64_module ? 96 : 112));
		auto name = exp ? word(size_t(exp) + 12) : 0;
		std::string module = "<image>";
		if (name && name < bytes.size()) {
			auto end = std::find(bytes.begin() + name, bytes.end(), uint8_t(0));
			if (end != bytes.end()) module.assign(reinterpret_cast<const char *>(bytes.data() + name), size_t(end - (bytes.begin() + name)));
		}
		fprintf(stderr, "Raven image pid=%d base=%llx size=%zu module=%s\n", getpid(), (unsigned long long)address, n, module.c_str());
	}
	for (size_t at = 0; at < n; at += 4096) state->pages[address + at] = PROT_NONE;
	for (const auto &mapping : image.mappings())
		for (uint64_t at = mapping.address; at < mapping.address + mapping.size; at += 4096) state->pages[at] = mapping.protection;
	// Register the view before changing protections; FEX tracks executable pages.
	uint64_t args[8]{state->host(address, n), n, 0x3000, 0x40, 1, 0};
	notify_memory("NotifyMemoryAlloc", args, 4);
	// Notify once per contiguous protection run, rather than once per guest
	// page. Large Chrome images otherwise cross the PE host boundary tens of
	// thousands of times and protect each 16 KiB host page up to four times.
	// All logical page protections are installed above before any host change,
	// so mixed-protection host pages still use the same union policy.
	for (size_t at = 0; at < n;) {
		const int protection = state->pages.at(address + at);
		size_t end = at + 4096;
		while (end < n && state->pages.at(address + end) == protection) end += 4096;
		protect_memory(address + at, end - at, protection);
		at = end;
	}
}
std::pair<uint64_t, uint64_t> FexProcess::stack_bounds() const { return {state->guest(state->stack), state->stack_top}; }
NativeMode FexProcess::mode() const { return state->mode; }
bool FexProcess::active() const { return state->active; }
bool FexProcess::stopped() const { return !state->parallel || state->workers.at(state->selected)->stopped.load(std::memory_order_acquire); }
bool FexProcess::parallel() const { return state->parallel; }
uint64_t FexProcess::return_gate() const { return state->guest(state->gate); }
uint64_t FexProcess::unix_call_gate() const { return state->guest(state->gate) + 64; }
raven_control *FexProcess::darwin_control() const { return unix_control; }
uint32_t FexProcess::copy_unix_port(uint32_t port) const
{
	return mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS ? port : 0;
}
uint64_t FexProcess::load_unix_library(const std::string &path, uint32_t count)
{
	state->require_owner();
	const bool mtld3d = std::filesystem::path(path).filename() == "mtld3d.so";
	if (wow64_module && !mtld3d) throw std::runtime_error("FEX WoW64 translator is only defined for mtld3d");
	if ((!mtld3d && std::filesystem::path(path).filename() != "winemetal.so") ||
	    (mtld3d ? count != 22 : mullion_selected() ? count != 197 : count != 132 && count != 146))
		throw std::runtime_error("unsupported FEX unix library/table pair");
	for (size_t i = 0; i < unix_libraries.size(); ++i)
		if (unix_libraries[i].path == path) {
			if (unix_libraries[i].count != count) throw std::runtime_error("FEX cached unix table count mismatch");
			return i + 1;
		}
	if (mtld3d) {
		// No DXMT ordinal wrappers: mtld3d owns a distinct 22-entry ABI.
		const char *root = getenv("RAVEN_MTLD3D_ARM64_ROOT");
		if (!root || !*root) throw std::runtime_error("set RAVEN_MTLD3D_ARM64_ROOT to the private arm64 overlay");
		uint32_t overlay_count = 0;
		if (!std::filesystem::is_regular_file(std::filesystem::path(root) / "mtld3d.so"))
			throw std::runtime_error("overlay missing at " + (std::filesystem::path(root) / "mtld3d.so").string());
		std::ifstream metadata(std::string(root) + "/unix-call-count");
		if (!(metadata >> overlay_count) || overlay_count != count)
			throw std::runtime_error("FEX mtld3d PE/unix table count mismatch");
		void *adapter = dlopen((std::string(root) + "/raven-mtld3d.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!adapter) throw std::runtime_error(dlerror());
		std::unique_ptr<void, decltype(&dlclose)> adapter_guard(adapter, dlclose);
		auto init = reinterpret_cast<void (*)(raven_control *)>(dlsym(adapter, "raven_mtld3d_init"));
		auto version = reinterpret_cast<uint32_t (*)()>(dlsym(adapter, "raven_mtld3d_protocol_version"));
		if (!init || !version || version() != RAVEN_GUEST_VERSION) throw std::runtime_error("mtld3d adapter protocol mismatch");
		void *image = dlopen((std::string(root) + "/mtld3d.so").c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!image) throw std::runtime_error(dlerror());
		std::unique_ptr<void, decltype(&dlclose)> image_guard(image, dlclose);
		auto get_contract = reinterpret_cast<const raven_unix_contract *(*)()>(dlsym(image, "raven_unix_get_contract"));
		const auto *contract = get_contract ? get_contract() : nullptr;
		if (!contract || contract->version != RAVEN_UNIX_CONTRACT_VERSION || contract->count != count ||
			!contract->argument_sizes || !contract->validate || !contract->set_memory_check)
			throw std::runtime_error("FEX unixlib compiled contract/count mismatch; reprovision overlay");
		init(unix_control);
		contract->set_memory_check(unix_check_range);
		auto table = reinterpret_cast<int32_t (**)(void *)>(dlsym(image, wow64_module ? "__wine_unix_call_wow64_funcs" : "__wine_unix_call_funcs"));
		if (!table) throw std::runtime_error("FEX mtld3d table missing");
		raven_mtld3d_callbacks callbacks{1, sizeof(raven_mtld3d_callbacks),
			reinterpret_cast<decltype(callbacks.create)>(dlsym(adapter, "raven_mtld3d_layer_create")),
			reinterpret_cast<decltype(callbacks.release)>(dlsym(adapter, "raven_mtld3d_layer_release")),
			reinterpret_cast<decltype(callbacks.present)>(dlsym(adapter, "raven_mtld3d_present"))};
		auto bind = reinterpret_cast<int (*)(const raven_mtld3d_callbacks *)>(dlsym(image, "raven_mtld3d_bind"));
		if (!bind || !callbacks.create || !callbacks.release || !callbacks.present || bind(&callbacks))
			throw std::runtime_error("mtld3d explicit callback binding failed");
		auto set_base = reinterpret_cast<void (*)(uint64_t)>(dlsym(image, "raven_mtld3d_set_guest_base"));
		if (!set_base) throw std::runtime_error("mtld3d address translator missing");
		set_base(wow64_module ? wow64_guest_base : 0);
		register_unix_code(reinterpret_cast<void *>(table[0]));
		register_unix_code(reinterpret_cast<void *>(init));
		if (void *copy=dlsym(image,"raven_unix_copy")) register_unix_code(copy);
		unix_libraries.push_back({path, image, table, count, contract});
		adapter_guard.release();
		image_guard.release();
		return unix_libraries.size();
	}
	const std::string selected_root = mullion_selected() ? (mullion_root() / "unix-arm64").string() : "";
	const char *root = mullion_selected() ? selected_root.c_str() : getenv("RAVEN_DXMT_ARM64_ROOT");
	if (!root || !*root) root = RAVEN_DXMT_DEFAULT_ARM64_ROOT;
	if (!*root) throw std::runtime_error("set RAVEN_DXMT_ARM64_ROOT, or configure it at build time, to the arm64 DXMT overlay");
	uint32_t overlay_count = 0;
	if (!std::filesystem::is_regular_file(std::filesystem::path(root) / "winemetal.so"))
		throw std::runtime_error("overlay missing at " + (std::filesystem::path(root) / "winemetal.so").string());
	std::ifstream metadata(std::string(root) + "/unix-call-count");
	if (!(metadata >> overlay_count) || overlay_count != count)
		throw std::runtime_error("FEX DXMT PE/unix table count mismatch");
	void *adapter = dlopen((std::string(root) + "/raven-dxmt.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!adapter) throw std::runtime_error(dlerror());
	std::unique_ptr<void, decltype(&dlclose)> adapter_guard(adapter, dlclose);
	auto version = reinterpret_cast<uint32_t (*)()>(dlsym(adapter, "raven_unixlib_protocol_version"));
	auto init = reinterpret_cast<void (*)(raven_control *)>(dlsym(adapter, "raven_unixlib_init"));
	auto wrap = reinterpret_cast<void *(*)(void *, uint32_t)>(dlsym(adapter, "raven_unixlib_wrap_table"));
	if (!version || version() != RAVEN_GUEST_VERSION || !init || !wrap) throw std::runtime_error("FEX DXMT adapter protocol mismatch");
	void *image = dlopen((std::string(root) + "/winemetal.so").c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!image) throw std::runtime_error(dlerror());
	std::unique_ptr<void, decltype(&dlclose)> image_guard(image, dlclose);
	auto get_contract = reinterpret_cast<const raven_unix_contract *(*)()>(dlsym(image, "raven_unix_get_contract"));
	const auto *contract = get_contract ? get_contract() : nullptr;
	if (!contract || contract->version != RAVEN_UNIX_CONTRACT_VERSION || contract->count != count ||
		!contract->argument_sizes || !contract->validate || !contract->set_memory_check)
		throw std::runtime_error("FEX unixlib compiled contract/count mismatch; reprovision overlay");
	contract->set_memory_check(unix_check_range);
	void *table = dlsym(image, "__wine_unix_call_funcs");
	if (!table) throw std::runtime_error("FEX DXMT table missing");
	init(unix_control);
	table = wrap(table, count);
	if (!table) throw std::runtime_error("FEX DXMT table wrapping failed");
	register_unix_code(reinterpret_cast<void *>(reinterpret_cast<int32_t (**)(void *)>(table)[0]));
	register_unix_code(reinterpret_cast<void *>(contract->validate));
	if (void *copy=dlsym(image,"raven_unix_copy")) register_unix_code(copy);
	unix_libraries.push_back({path, image, reinterpret_cast<int32_t (**)(void *)>(table), count, contract});
	read_native_buffer = reinterpret_cast<decltype(read_native_buffer)>(dlsym(adapter, "raven_dxmt_read_memory"));
	adapter_guard.release();
	image_guard.release();
	return unix_libraries.size();
}
void FexProcess::add_api_trap(uint64_t address, uint32_t callee_cleanup)
{
	if (state->mode == NativeMode::x64 && callee_cleanup) throw std::runtime_error("FEX x64 API cannot use stdcall cleanup");
	state->traps.emplace(address, callee_cleanup);
}
size_t FexProcess::callback_depth() const { return state->thread.callbacks.size(); }
NativeContext FexProcess::callback_origin(size_t index) const { return state->thread.callbacks.at(index); }
uint64_t FexProcess::result_register() const { return context(false).registers.rax; }
void FexProcess::set_resume_hook(std::function<void()> hook) { state->resume_hook = std::move(hook); }
std::vector<uint64_t> FexProcess::api_arguments(size_t count) const
{
	FexCallTimer timing{2};
	if (state->thread.translated_words) return *state->thread.translated_words;
	if (wow64_module) {
		std::vector<uint64_t> words;
		auto bytes = read_memory(context(false).registers.rsp + 4, count * 4);
		for (size_t i = 0; i < count; ++i) { uint32_t word; memcpy(&word, bytes.data() + i * 4, 4); words.push_back(word); }
		return words;
	}
	const auto r = context(false).registers;
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
	if (mode() != NativeMode::x64 || !stopped() || !state->active || state->thread.awaiting_api || !state->thread.awaiting_syscall)
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
	// API return changes only GPRs; preserve the live FP/YMM state in FEX.
	auto saved = context(false);
	if (wow64_module) {
		uint32_t target; auto bytes = read_memory(saved.registers.rsp, 4); memcpy(&target, bytes.data(), 4);
		saved.registers.rsp += 4 + state->traps.at(saved.registers.rip - 1);
		saved.registers.rip = target; saved.registers.rax = uint32_t(value);
		saved.registers.rdx = uint32_t(value >> 32);
		set_context(saved); state->thread.awaiting_api = false; return;
	}
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
	struct RestoreStackTop {
		uint64_t &top;
		uint64_t saved;
		~RestoreStackTop() { top = saved; }
	} restore{state->thread.stack_top, state->thread.stack_top};
	state->thread.stack_top = outer.registers.rsp - 256;
	state->active = false;
	return call(address, arguments, timeout);
}
NativeThreadState FexProcess::save_thread(bool snapshot) const
{
	static fex_perf::Counter perf{"save_thread"};
	fex_perf::Timer timer(perf);
	auto saved = state->thread;
	saved.stopped = stopped();
	saved.context_in_task = state->parallel && !snapshot;
	if (saved.stopped) {
		saved.context = context();
	} else {
		if (wow64_module) saved.context.registers.fs_base = state->teb;
		else saved.context.registers.gs_base = state->teb;
	}
	saved.active_call = state->active;
	return saved;
}
void FexProcess::load_thread(const NativeThreadState &saved)
{
	if (state->parallel) state->selected = saved.task;
	state->thread = saved;
	state->teb = wow64_teb = wow64_module ? saved.context.registers.fs_base : saved.context.registers.gs_base;
	state->active = saved.active_call;
	if (saved.stopped && !saved.context_in_task) set_context(saved.context);
}
NativeThreadState FexProcess::new_thread(uint64_t stack_top, uint64_t teb)
{
	NativeThreadState thread;
	thread.stack_top = stack_top;
	if (state->parallel) thread.task = state->make_worker();
	thread.context = state->initial;
	if (wow64_module) thread.context.registers.fs_base = teb;
	else thread.context.registers.gs_base = teb;
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
void FexProcess::drain_unix_events(const std::function<void(uint64_t)> &signal)
{
	for (auto &slot : unix_control->unix_events)
		if (auto handle = __atomic_exchange_n(&slot, 0, __ATOMIC_ACQ_REL)) signal(handle);
}
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
		if (remaining.count() <= 0) return {NativeStop::timeout, 0, context(false).registers.rip, 0};
		// GPU callbacks publish native events even when no guest thread is runnable.
		// Service them periodically while the current guest spins in GetData.
		const bool slicing = !state->parallel && state->slice.count() > 0 && state->slice_wanted && state->slice_yield &&
			(!unix_libraries.empty() || state->slice_wanted());
		// Short API calls repeatedly return before the per-entry timer fires.
		// Account for their accumulated wall time so GetTickCount/GetData loops
		// cannot starve the worker which completes the pending GPU query.
		if (slicing && std::chrono::steady_clock::now() - state->slice_started >= state->slice) {
			state->slice_started = std::chrono::steady_clock::now();
			state->slice_yield();
			state->slice_started = std::chrono::steady_clock::now();
			continue; // Recompute the deadline after another guest thread ran.
		}
		state->budget = slicing ? std::min(remaining, state->slice) : remaining;
		const bool sampling = state->sample_period.count() > 0 && state->sampler;
		if (sampling) state->budget = std::min(state->budget, state->sample_period);
		auto result = resume();
		if (sampling && result.stop == NativeStop::timeout) state->sampler(result.instruction);
		if (sampling && result.stop == NativeStop::timeout && state->budget < remaining && !slicing) continue;
		if (state->parallel && result.stop == NativeStop::timeout && std::chrono::steady_clock::now() < deadline)
			continue; // A remote suspend consumed this doorbell; resume the edited packet.
		if (result.stop != NativeStop::timeout || !slicing || state->budget >= remaining) return result;
		state->slice_started = std::chrono::steady_clock::now();
		state->slice_yield();
		state->slice_started = std::chrono::steady_clock::now();
	}
}
void FexProcess::set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t)> sampler)
{
	state->sample_period = period;
	state->sampler = std::move(sampler);
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
	if (size == 8) __atomic_compare_exchange_n(reinterpret_cast<uint64_t *>(state->host(address, size)), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	else if (size == 4) { uint32_t word = uint32_t(expected); __atomic_compare_exchange_n(reinterpret_cast<uint32_t *>(state->host(address, size)), &word, uint32_t(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); expected = word; }
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
uint64_t FexProcess::map_shared(int descriptor, size_t size, uint64_t offset, uint64_t requested)
{
	if (offset % 4096 || offset > INT64_MAX || !size || size > size_t(INT64_MAX - offset))
		throw std::runtime_error("FEX invalid shared mapping offset or size");
	const size_t delta = offset % 16384;
	if (requested && (requested < delta || (requested - delta) % 16384))
		throw std::runtime_error("FEX fixed shared mapping has incompatible page alignment");
	// FEX translates KUSER_SHARED_DATA through this dedicated host page. Share
	// the same descriptor as the clock writer, preserving the guest read-only policy.
	if (requested == 0x7ffe0000 && size == 4096) {
		void *mapped = mmap(user_shared_data.data(), user_shared_data.size(), PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, descriptor, off_t(offset));
		if (mapped == MAP_FAILED) throw std::runtime_error("FEX shared user data mapping failed");
		if (state->window) {
			// WoW64 guest addresses resolve inside its relocated window. Keep
			// that view on the same descriptor as the module mirror and clocks.
			const auto address = map_memory(size, PROT_READ | PROT_WRITE, requested);
			mapped = mmap(reinterpret_cast<void *>(state->host(address, size)),
				state->regions.at(address).size, PROT_READ | PROT_WRITE,
				MAP_FIXED | MAP_SHARED, descriptor, off_t(offset));
			if (mapped == MAP_FAILED) throw std::runtime_error("WoW64 shared user data mapping failed");
		}
		state->pages[requested] = PROT_READ | PROT_WRITE;
		return requested;
	}
	// Map the containing host pages and return the pixel/view start, not the
	// preceding file header. Keep the reservation base for exact teardown.
	const auto address = map_memory(size + delta, PROT_READ | PROT_WRITE, requested ? requested - delta : 0);
	const auto n = state->regions.at(address).size;
	void *mapped = mmap(reinterpret_cast<void *>(state->host(address, n)), n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, descriptor, off_t(offset - delta));
	if (mapped == MAP_FAILED) { unmap_memory(address); throw std::runtime_error("FEX shared descriptor mapping failed"); }
	// Logical permissions remain unchanged. FEX must validate executable bytes
	// in shared views because writes through aliases/other processes bypass
	// this virtual address's dirty notifications and page faults.
	uint64_t shared[8]{state->host(address, n), n};
	raven_ec_call(mapped_image->export_address("RavenMarkSharedMemory"), shared, state->environment()->teb.data());
	if (delta) {
		try {
			protect_memory(address, delta, PROT_NONE);
			state->shared_view_bases.emplace(address + delta, address);
		}
		catch (...) { unmap_memory(address); throw; }
	}
	return address + delta;
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
void FexProcess::resume_context_thread(NativeThreadState &thread)
{
	if (!state->parallel) return;
	auto &worker = *state->workers.at(thread.task);
	const auto caller = save_thread(true);
	try {
		load_thread(thread);
		set_context(thread.context);
	} catch (...) {
		load_thread(caller);
		throw;
	}
	load_thread(caller);
    // A racing syscall/exception remains queued for scheduler dispatch, but its
    // authoritative packet must include the edit before the snapshot is discarded.
    thread.stopped = false;
    thread.context_in_task = true;
    if (!thread.context_pause_interrupt) return;
	{
		std::lock_guard lock(worker.mutex);
		if (wow64_module) __atomic_store_n(worker.interrupt, 0u, __ATOMIC_RELEASE);
		else __atomic_store_n(worker.environment->cpu.doorbell, 0u, __ATOMIC_RELEASE);
		worker.stopped.store(false, std::memory_order_release);
		worker.requested = true;
	}
	thread.stopped = false;
	thread.context_in_task = true;
	worker.wake.notify_all();
}
void FexProcess::pause_thread(NativeThreadState &thread)
{
	if (!state->parallel || thread.stopped) return;
	auto selected = state->selected;
	auto teb = state->teb;
	state->selected = thread.task;
	state->teb = wow64_module ? thread.context.registers.fs_base : thread.context.registers.gs_base;
	try {
		auto &worker = *state->workers.at(thread.task);
		worker.pause();
		thread.context_pause_interrupt = worker.result.kind == NativeStop::timeout;
		thread.context = context();
	} catch (...) {
		state->selected = selected;
		state->teb = teb;
		throw;
	}
	thread.stopped = true;
	thread.context_in_task = false;
	state->selected = selected;
	state->teb = teb;
}
}

namespace raven {
void FexProcess::set_teb(uint64_t base)
{
	if (mode() != NativeMode::x86) throw std::runtime_error("FEX x64 set_teb is unsupported");
	auto saved = context(); saved.registers.fs_base = base; set_context(saved);
}
}

static bool raven_unix_fault_pc(uintptr_t pc) {
    const auto count=unix_code_count.load(std::memory_order_acquire);
    for (unsigned i=0;i<count;++i)
        if (pc>=unix_code_ranges[i].begin && pc<unix_code_ranges[i].end) return true;
    return false;
}
