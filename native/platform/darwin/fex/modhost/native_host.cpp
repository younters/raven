// SPDX-License-Identifier: MIT
// M1c native host substrate. Refuses entry while any import/ABI gate is unmet.
#define RAVEN_HOST_LOADER
#include "pe_preflight.cpp"
#include "raven_host.h"
#include "abi.h"
#include "crt.h"
#include "sync.h"
#include "nt_strings.h"
#include "environment.h"
#include "vm.h"
#include "pe_view.h"
#include "nt_io.h"
#include "nt_system.h"
#include "format.h"
#include "nt_registry.h"
#include "nt_threads.h"
#include "context.h"
#include "unwind.h"
#include "fault.h"
#include "bench.h"
#include "module_contract.h"
#include <thread>
#include <atomic>
#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cassert>
#include <iomanip>

namespace {
using RavenVM::allocate;
using RavenVM::release;
using RavenVM::protect;
using RavenVM::query;
using RavenVM::MemoryInfo;
constexpr int32_t invalid = int32_t(0xc000000d);
size_t page_size() {
	return RavenVM::page();
}
void *heap_alloc(void *, uint32_t flags, size_t size) {
	if (flags & ~8u)
		return nullptr;
	return (flags & 8) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}
uint8_t heap_free(void *, uint32_t, void *p) {
	free(p);
	return 1;
}
int32_t qpc(int64_t *counter, int64_t *frequency) {
	if (!counter)
		return invalid;
	*counter = int64_t(mach_absolute_time());
	if (frequency) {
		mach_timebase_info_data_t timebase;
		mach_timebase_info(&timebase);
		*frequency = int64_t(1000000000ull * timebase.denom / timebase.numer);
	}
	return 1;
}
uint8_t rtl_qpc(int64_t *counter) {
	if (!counter)
		return 0;
	*counter = mach_absolute_time();
	return 1;
}
std::mutex locks_mutex;
std::map<void *, std::unique_ptr<std::recursive_mutex>> locks;
int32_t initialize_lock(void *key) {
	if (!key)
		return invalid;
	std::lock_guard<std::mutex> lock(locks_mutex);
	if (locks.count(key))
		return invalid;
	locks[key] = std::make_unique<std::recursive_mutex>();
	return 0;
}
std::recursive_mutex *get_lock(void *key) {
	std::lock_guard<std::mutex> lock(locks_mutex);
	auto it = locks.find(key);
	if (it == locks.end()) {
		std::cerr << "uninitialized critical section\n";
		std::abort();
	}
	return it->second.get();
}
int32_t enter(void *key) {
	get_lock(key)->lock();
	return 0;
}
int32_t leave(void *key) {
	get_lock(key)->unlock();
	return 0;
}
uint8_t try_enter(void *key) {
	return get_lock(key)->try_lock();
}
int32_t delete_lock(void *key) {
	std::lock_guard<std::mutex> lock(locks_mutex);
	return locks.erase(key) ? 0 : invalid;
}
void *resolve_native(const char *dll, const char *name) {
	if (std::string(dll) != "ntdll.dll")
		return nullptr;
#define ENTRY(n, f) \
	if (std::string(name) == n) \
	return reinterpret_cast<void *>(&f)
	ENTRY("NtAllocateVirtualMemory", allocate);
	ENTRY("NtAllocateVirtualMemoryEx", RavenVM::allocate_ex);
	ENTRY("NtFlushInstructionCache", RavenVM::flush);
	ENTRY("NtFreeVirtualMemory", release);
	ENTRY("NtProtectVirtualMemory", protect);
	ENTRY("NtQueryVirtualMemory", query);
	ENTRY("RtlAllocateHeap", heap_alloc);
	ENTRY("RtlFreeHeap", heap_free);
	ENTRY("RtlQueryPerformanceCounter", rtl_qpc);
	ENTRY("RtlInitializeCriticalSection", initialize_lock);
	ENTRY("RtlEnterCriticalSection", enter);
	ENTRY("RtlLeaveCriticalSection", leave);
	ENTRY("RtlTryEnterCriticalSection", try_enter);
	ENTRY("RtlDeleteCriticalSection", delete_lock);
#undef ENTRY
	if (auto *p = RavenCRT::resolve(name))
		return p;
	if (auto *p = RavenSync::resolve(name))
		return p;
	if (auto *p = RavenNT::resolve_strings(name))
		return p;
	if (auto *p = RavenNT::resolve_system(name))
		return p;
	if (auto *p = RavenFormat::resolve(name))
		return p;
	if (auto *p = RavenNT::resolve_registry(name))
		return p;
	if (auto *p = RavenNT::resolve_context(name))
		return p;
	return RavenNT::resolve_unwind(name);
}
NativeJit import_jit;
std::mutex adapter_mutex;
std::map<std::pair<std::string, std::string>, void *> adapter_cache;
void *resolve(const char *dll, const char *name) {
	std::lock_guard<std::mutex> lock(adapter_mutex);
	auto key = std::make_pair(std::string(dll), std::string(name));
	auto found = adapter_cache.find(key);
	if (found != adapter_cache.end())
		return found->second;
	void *adapter = nullptr;
	if (!strcmp(dll, "ntdll.dll") && !strcmp(name, "RtlCaptureContext"))
		adapter = reinterpret_cast<void *>(&RavenNT::raven_capture_ec_context);
	else if (!strcmp(dll, "ntdll.dll") && !strcmp(name, "RtlRestoreContext"))
		adapter = reinterpret_cast<void *>(&RavenNT::raven_restore_ec_context);
	else {
		unsigned stack = 0;
		auto *target = !strcmp(dll, "ntdll.dll") ? RavenNT::resolve_threads(name, stack) : nullptr;
		if (!target)
			target = resolve_native(dll, name);
		if (!target && !strcmp(dll, "ntdll.dll"))
			target = RavenNT::resolve_io(name, stack);
		if (target)
			adapter = native_adapter(import_jit, target, stack);
	}
	if (adapter) {
		if (RavenNT::active_process)
			RavenNT::active_process->mark_ec(uintptr_t(adapter), 4);
		adapter_cache.emplace(std::move(key), adapter);
	}
	return adapter;
}
std::unique_ptr<PEView> mapped_image;
std::unique_ptr<RavenNT::ProcessEnvironment> process_environment;
std::unique_ptr<RavenNT::ThreadEnvironment> thread_environment;
void inspect_image(Bytes image, size_t opt) {
	mapped_image = std::make_unique<PEView>(PEView{image, opt});
	RavenNT::active_image = mapped_image.get();
	process_environment = std::make_unique<RavenNT::ProcessEnvironment>();
	thread_environment = std::make_unique<RavenNT::ThreadEnvironment>(*process_environment);
	RavenNT::active_process = process_environment.get();
	RavenNT::active_thread = thread_environment.get();
	RavenNT::import_lookup = resolve;
	RavenNT::pseudo_handle = RavenNT::current_handle;
	process_environment->put(0x10, image.data);
	mapped_image->install_tls(*thread_environment);
	auto *entry = mapped_image->export_address("RavenProcessInit");
	if (!entry || uintptr_t(entry) != uintptr_t(image.data) + RavenContract::process_init_rva ||
		!mapped_image->ec_code(RavenContract::process_init_rva) ||
		image.get<uint32_t>(RavenContract::process_init_rva) !=
			RavenContract::process_init_instruction)
		throw std::runtime_error("CHPE RavenProcessInit target/RVA does not match pinned contract");
	std::cout << "CHPE native RavenProcessInit RVA=" << std::hex
				<< uintptr_t(entry) - uintptr_t(image.data) << std::dec
				<< " TLS_bytes=" << thread_environment->static_tls.size()
				<< " TEB/CPU-area prepared (not entered)\n";
}
extern "C" void raven_check_ec_call();
extern "C" void raven_checked_import();
alignas(16384) std::array<uint8_t, 16384> user_shared_data{};
NativeJit callback_jit;
sigjmp_buf guest_return;
sigjmp_buf *active_return = &guest_return;
bool exercise_nested = false, nested_done = false;
uint64_t nested_result = 0, nested_code = 0;
uint64_t expected_resumed = 4884;
unsigned nested_calls = 0;
uint64_t timed_calls = 0, expected_timed_calls = 100000;
uint64_t integer_expected = 5292597211099207303ull;
uint64_t float_expected_bits = 0;
bool x87_test = false;
unsigned x87_dispatches = 0;
uint64_t invoke_nested(void *packet);
bool guest_active = false, guest_ok = false;
unsigned guest_calls = 0;
uint64_t expected_guest_end = 0;
uint64_t expected_fault_rip = 0, expected_fault_address = 0;
unsigned faults_repaired = 0;
uint64_t guest_dispatch(uint64_t number, void *packet) {
	require_native_x18();
	auto *context = static_cast<uint8_t *>(packet);
	auto reg = [&](size_t offset) {
		uint64_t v;
		memcpy(&v, context + offset, 8);
		return v;
	};
	if (!guest_active)
		std::abort();
	if (number == 0x100) {
		++guest_calls;
		if (reg(0xc8) != 7 || reg(0x88) != 11 || reg(0xb8) != 13 || reg(0xc0) != 17) {
			guest_ok = false;
			siglongjmp(*active_return, 1);
		}
		auto result = reg(0xc8) + 3 * reg(0x88) + 5 * reg(0xb8) + 7 * reg(0xc0);
		if (exercise_nested)
			result += invoke_nested(packet);
		return result;
	}
	if (number == 0x108) {
		uint16_t status;
		memcpy(&status, context + 0x102, 2);
		if (!x87_test || (status & 0x3900) != 0x3100 || context[0x104] != 0xc0) {
			guest_ok = false;
			siglongjmp(*active_return, 1);
		}
		++x87_dispatches;
		return 0;
	}
	if (number == 0x109) {
		uint16_t status;
		memcpy(&status, context + 0x102, 2);
		double value = 4.14159265358979323846;
		uint64_t bits;
		memcpy(&bits, &value, 8);
		guest_ok = x87_dispatches == 1 && faults_repaired == 1 && !(status & 0x3800) &&
			context[0x104] == 0 && reg(0xc8) == bits && reg(0xf8) == expected_guest_end;
		siglongjmp(*active_return, 1);
	}
	if (number == 0x104 || number == 0x105 || number == 0x107) {
		if (number == 0x104)
			guest_ok = reg(0xc8) == integer_expected;
		else if (number == 0x105)
			guest_ok = reg(0x1a0) == float_expected_bits;
		else
			guest_ok = reg(0xc8) == expected_timed_calls && timed_calls == expected_timed_calls;
		guest_ok = guest_ok && reg(0xf8) == expected_guest_end;
		siglongjmp(*active_return, 1);
	}
	if (number == 0x106) {
		++timed_calls;
		return 1;
	}
	if (number == 0x103) {
		nested_result = reg(0xc8);
		nested_done = true;
		++nested_calls;
		siglongjmp(*active_return, 1);
	}
	if (number == 0x102) {
		guest_ok = faults_repaired == 1 && reg(0xc8) == 4665 && reg(0xf8) == expected_guest_end;
		siglongjmp(*active_return, 1);
	}
	if (number == 0x101) {
		guest_ok =
			guest_calls == 1 && reg(0xc8) == expected_resumed && reg(0xf8) == expected_guest_end;
		siglongjmp(*active_return, 1);
	}
	guest_ok = false;
	siglongjmp(*active_return, 1);
}
int32_t unix_call(uint64_t, uint32_t, void *) {
	require_native_x18();
	return RavenNT::not_supported;
}
int32_t exception_callback(void *opaque, void *packet, void *) {
	require_native_x18();
	auto *record = static_cast<RavenFault::Record *>(opaque);
	auto *context = static_cast<uint8_t *>(packet);
	uint64_t rip = 0;
	memcpy(&rip, context + 0xf8, 8);
	if (record->code != 0xc0000005 || record->count != 2 || record->information[0] != 0 ||
		record->information[1] != expected_fault_address || rip != expected_fault_rip ||
		record->address != reinterpret_cast<void *>(rip))
		return RavenNT::not_supported;
	if (x87_test) {
		uint16_t status;
		memcpy(&status, context + 0x102, 2);
		if ((status & 0x3900) != 0x3100 || context[0x104] != 0xc0)
			return RavenNT::not_supported;
	}
	++faults_repaired;
	rip += 3;
	uint64_t value = 0x1234;
	memcpy(context + 0xf8, &rip, 8);
	memcpy(context + 0x78, &value, 8);
	return 0;
}
void *cpu_area(void *teb) {
	require_native_x18();
	void *area = nullptr;
	if (teb)
		memcpy(&area, static_cast<uint8_t *>(teb) + 0x1788, 8);
	return area;
}
void host_log(const char *message) {
	require_native_x18();
	std::cerr << "FEX: " << message << std::flush;
}
uint32_t jit_protect(uint32_t mode) {
	require_native_x18();
	auto previous = NativeJit::protect(mode);
	if (getenv("RAVEN_MODHOST_TRACE"))
		fprintf(stderr, "jit mode=%u previous=%u\n", mode, previous);
	return previous;
}
void *native_jit(uint64_t size) {
	require_native_x18();
	auto *p = callback_jit.allocate(size);
	process_environment->mark_ec(uintptr_t(p), size);
	return p;
}
void memory_event(uint64_t address, uint64_t size, uint64_t kind) {
	require_native_x18();
	// FEX explicitly identifies native cache reservations that may overcommit.
	// Darwin mprotect commits access without physically populating demand-zero pages.
	if (kind == 1) {
		void *base = reinterpret_cast<void *>(address);
		size_t n = size;
		if (RavenVM::allocate(reinterpret_cast<void *>(-1), &base, 0, &n, 0x1000, 4))
			std::abort();
	} else if (kind != 2)
		std::abort();
}
uint64_t invoke_nested(void *packet) {
	struct Frame {
		sigjmp_buf completion;
		alignas(16) uint8_t outer_context[0x4d0];
		uint64_t stack_base, stack_limit;
	};
	auto frame = std::make_unique<Frame>();
	memcpy(frame->outer_context, packet, sizeof(frame->outer_context));
	auto &cpu = thread_environment->cpu;
	frame->stack_base = cpu.stack_base;
	frame->stack_limit = cpu.stack_limit;
	void *emulator_stack = nullptr, *stack = nullptr;
	size_t emulator_size = 262144, stack_size = 262144;
	if (allocate(nullptr, &emulator_stack, 0, &emulator_size, 0x3000, 4) ||
		allocate(nullptr, &stack, 0, &stack_size, 0x3000, 4))
		std::abort();
	cpu.stack_limit = uintptr_t(emulator_stack);
	cpu.stack_base = cpu.stack_limit + emulator_size;
	auto *context = static_cast<uint8_t *>(packet);
	auto sp = uintptr_t(stack) + stack_size - 128;
	uint64_t argument = 41;
	memcpy(context + 0xf8, &nested_code, 8);
	memcpy(context + 0x98, &sp, 8);
	memcpy(context + 0x80, &argument, 8);
	auto *previous = active_return;
	active_return = &frame->completion;
	nested_done = false;
	uint64_t args[8]{};
	if (!sigsetjmp(frame->completion, 1))
		raven_ec_call(
			mapped_image->export_address("BeginSimulation"), args, thread_environment->teb.data());
	active_return = previous;
	cpu.stack_base = frame->stack_base;
	cpu.stack_limit = frame->stack_limit;
	memcpy(packet, frame->outer_context, sizeof(frame->outer_context));
	size_t zero = 0;
	release(nullptr, &emulator_stack, &zero, 0x8000);
	zero = 0;
	release(nullptr, &stack, &zero, 0x8000);
	if (!nested_done || nested_result != 124)
		std::abort();
	return nested_result;
}
void initialize_image(Bytes image, size_t opt, uint64_t (*dispatch)(uint64_t, void *) = guest_dispatch,
	int32_t (*exception)(void *, void *, void *) = exception_callback) {
	PEView view{image, opt};
	auto &process = *process_environment;
	RavenVM::code_event =
		+[](uintptr_t p, size_t n, bool present) { process_environment->mark_ec(p, n, present); };
	auto metadata = view.metadata();
	auto mark = [&](void *base, size_t size) { process.mark_ec(uintptr_t(base), size); };
	import_jit.each_region(mark);
	callback_jit.each_region(mark);
	process.mark_ec(uintptr_t(&raven_check_ec_call), 4);
	process.mark_ec(uintptr_t(&RavenNT::raven_capture_ec_context), 4);
	process.mark_ec(uintptr_t(&RavenNT::raven_restore_ec_context), 4);
	auto map = image.get<uint32_t>(metadata + 4), count = image.get<uint32_t>(metadata + 8);
	for (uint32_t i = 0; i < count; ++i) {
		auto start = image.get<uint32_t>(map + i * 8), size = image.get<uint32_t>(map + i * 8 + 4);
		if ((start & 3) == 1)
			process.mark_ec(uintptr_t(image.data) + (start & ~3u), size);
	}
	auto set_slot = [&](unsigned offset, void *target) {
		auto rva = image.get<uint32_t>(metadata + offset);
		if (rva)
			image.put<uint64_t>(rva, uintptr_t(target));
	};
	set_slot(20, view.export_address("ExitToX64"));
	set_slot(24, view.export_address("RetToEntryThunk"));
	for (auto offset : {28, 32, 36})
		set_slot(offset, reinterpret_cast<void *>(&raven_check_ec_call));
	static RavenHostV1 host{};
	host.size = sizeof(host);
	host.version = 1;
	host.shared_data = user_shared_data.data();
	auto callback = [&](auto fn) {
		auto *adapter = native_adapter(callback_jit, reinterpret_cast<void *>(fn));
		mark(adapter, 32);
		return adapter;
	};
	host.dispatch = reinterpret_cast<decltype(host.dispatch)>(callback(dispatch));
	host.unix_call = reinterpret_cast<decltype(host.unix_call)>(callback(unix_call));
	host.exception = reinterpret_cast<decltype(host.exception)>(callback(exception));
	host.cpu_area = reinterpret_cast<decltype(host.cpu_area)>(callback(cpu_area));
	host.log = reinterpret_cast<decltype(host.log)>(callback(host_log));
	host.jit_write_protect =
		reinterpret_cast<decltype(host.jit_write_protect)>(callback(jit_protect));
	host.allocate_native_jit =
		reinterpret_cast<decltype(host.allocate_native_jit)>(callback(native_jit));
	host.memory_event = reinterpret_cast<decltype(host.memory_event)>(callback(memory_event));
	auto pe = opt - 24;
	auto sections = image.get<uint16_t>(pe + 6), optional = image.get<uint16_t>(pe + 20);
	for (unsigned i = 0; i < sections; ++i) {
		auto section = opt + optional + i * 40;
		auto flags = image.get<uint32_t>(section + 36);
		if (!(flags & 0x20000000))
			continue;
		auto rva = image.get<uint32_t>(section + 12), size = image.get<uint32_t>(section + 8);
		if (rva % 16384)
			throw std::runtime_error("PE code section is not 16K aligned");
		sys_icache_invalidate(image.data + rva, size);
		if (mprotect(image.data + rva, (size + 16383) & ~size_t(16383), PROT_READ | PROT_EXEC))
			throw std::runtime_error("PE code mprotect failed");
	}
	uint64_t args[8]{uintptr_t(image.data), 1, 0};
	auto teb = thread_environment->teb.data();
	auto entry = image.get<uint32_t>(opt + 16);
	if (!entry)
		throw std::runtime_error("missing PE DllMain entry");
	{
		auto result = raven_ec_call(image.data + view.native_rva(entry), args, teb);
		if (!result)
			throw std::runtime_error("DllMain rejected attach");
		std::cout << "PASS: native PE DllMain attach\n" << std::flush;
	}
	args[0] = uintptr_t(&host);
	auto status = uint32_t(raven_ec_call(view.export_address("RavenProcessInit"), args, teb));
	if (status)
		throw std::runtime_error("RavenProcessInit returned " + std::to_string(status));
	std::cout << "PASS: RavenProcessInit\n" << std::flush;
	memset(args, 0, sizeof(args));
	status = uint32_t(raven_ec_call(view.export_address("ThreadInit"), args, teb));
	if (status)
		throw std::runtime_error("ThreadInit returned " + std::to_string(status));
	std::cout << "PASS: ThreadInit\n" << std::flush;
}
void verify_host_callret_fault() {
	// The pinned frontend allocates one 4 MiB call/return stack between two
	// 16 KiB guards. Use its real allocation, not an arbitrary dummy address.
	uintptr_t guard = 0;
	unsigned matches = 0;
	for (const auto &[base, region] : RavenVM::regions)
		if (region.size == 0x400000 + 2 * 16384 && region.initial == 1 &&
			!region.protections.front() && !region.protections.back()) {
			guard = base;
			++matches;
		}
	if (matches != 1) throw std::runtime_error("call/return guard probe did not find the pinned allocation");
	RavenFault::Record record{};
	record.code = 0xc0000005;
	record.count = 2;
	record.information[1] = guard;
	RavenFault::NativeContext native{};
	native.pc = uintptr_t(&host_log); // Native Mach-O code, outside the translator.
	native.x[17] = 0x12345678;
	const auto before = native;
	uint64_t args[8]{uintptr_t(&record), uintptr_t(&native)};
	const auto status = uint32_t(raven_ec_call(mapped_image->export_address("RavenHandleFault"), args,
		thread_environment->teb.data()));
	if (status != uint32_t(0xc00000bb) || memcmp(&before, &native, sizeof(native)))
		throw std::runtime_error("non-JIT call/return-range host fault was repaired or changed context");
	std::cout << "PASS: non-JIT host fault in actual call/return guard rejected without context changes\n";
}
void execute_image(Bytes image, size_t opt) {
	initialize_image(image, opt);
	verify_host_callret_fault();
	PEView view{image, opt};
	uint64_t args[8]{};
	auto teb = thread_environment->teb.data();
	const uint8_t code[]{0xb9, 7, 0, 0, 0, 0xba, 11, 0, 0, 0, 0x41, 0xb8, 13, 0, 0, 0, 0x41, 0xb9,
		17, 0, 0, 0, 0x49, 0x89, 0xca, 0xb8, 0, 1, 0, 0, 0x0f, 0x05, 0x48, 0x05, 0x34, 0x12, 0, 0,
		0x49, 0x89, 0xc2, 0xb8, 1, 1, 0, 0, 0x0f, 0x05, 0xeb, 0xfe};
	void *guest_code = nullptr, *guest_stack = nullptr;
	size_t code_size = sizeof(code), stack_size = 262144;
	if (allocate(nullptr, &guest_code, 0, &code_size, 0x3000, 0x40) ||
		allocate(nullptr, &guest_stack, 0, &stack_size, 0x3000, 4))
		throw std::runtime_error("guest allocation failed");
	memcpy(guest_code, code, sizeof(code));
	uint64_t notification[8]{uintptr_t(guest_code), code_size, 0x3000, 0x40, 1, 0};
	raven_ec_call(view.export_address("NotifyMemoryAlloc"), notification, teb);
	auto *context = thread_environment->context.data();
	auto rip = uintptr_t(guest_code), rsp = uintptr_t(guest_stack) + stack_size - 128;
	memcpy(context + 0xf8, &rip, 8);
	memcpy(context + 0x98, &rsp, 8);
	expected_guest_end = rip + sizeof(code) - 2;
	guest_calls = 0;
	guest_ok = false;
	uint64_t previous_teb;
	asm volatile("mov %0,x18" : "=r"(previous_teb));
	if (!sigsetjmp(guest_return, 1)) {
		guest_active = true;
		NativeJit::protect(1);
		raven_ec_call(view.export_address("BeginSimulation"), args, teb);
		throw std::runtime_error("BeginSimulation unexpectedly returned");
	}
	asm volatile("mov x18,%0" ::"r"(previous_teb));
	guest_active = false;
	if (!guest_ok)
		throw std::runtime_error("guest dispatch/resume result mismatch");
	std::cout
		<< "PASS: x86-64 dispatch args=7,11,13,17 host_result=224 resumed_result=4884 calls=1\n"
		<< std::flush;
	RavenFault::Scope fault_scope(view.export_address("RavenHandleFault"));
	void *bad = nullptr;
	size_t bad_size = 16384;
	if (allocate(nullptr, &bad, 0, &bad_size, 0x2000, 1))
		throw std::runtime_error("fault reservation failed");
	uint8_t fault_code[]{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8b, 0x00, 0x48, 0x83, 0xc0, 5,
		0x49, 0x89, 0xc2, 0xb8, 2, 1, 0, 0, 0x0f, 0x05, 0xeb, 0xfe};
	auto bad_value = uintptr_t(bad);
	memcpy(fault_code + 2, &bad_value, 8);
	void *fault_guest = nullptr;
	size_t fault_size = sizeof(fault_code);
	if (allocate(nullptr, &fault_guest, 0, &fault_size, 0x3000, 0x40))
		throw std::runtime_error("fault guest allocation failed");
	memcpy(fault_guest, fault_code, sizeof(fault_code));
	notification[0] = uintptr_t(fault_guest);
	notification[1] = fault_size;
	raven_ec_call(view.export_address("NotifyMemoryAlloc"), notification, teb);
	rip = uintptr_t(fault_guest);
	memcpy(context + 0xf8, &rip, 8);
	memcpy(context + 0x98, &rsp, 8);
	expected_fault_rip = rip + 10;
	expected_fault_address = bad_value;
	expected_guest_end = rip + sizeof(fault_code) - 2;
	faults_repaired = 0;
	guest_ok = false;
	if (!sigsetjmp(guest_return, 1)) {
		guest_active = true;
		thread_environment->cpu.simulation = 1;
		raven_ec_call(view.export_address("BeginSimulation"), args, teb);
		std::abort();
	}
	asm volatile("mov x18,%0" ::"r"(previous_teb));
	guest_active = false;
	if (!guest_ok)
		throw std::runtime_error("precise AV repair/resume result mismatch");
	std::cout << "PASS: precise AV guest_rip=" << std::hex << expected_fault_rip
				<< " fault_address=" << expected_fault_address << std::dec
				<< " repaired_result=4665\n"
				<< std::flush;
	const uint8_t callback_code[]{0x48, 0x89, 0xc8, 0x48, 0x6b, 0xc0, 3, 0x48, 0x83, 0xc0, 1, 0x49,
		0x89, 0xc2, 0xb8, 3, 1, 0, 0, 0x0f, 0x05, 0xeb, 0xfe};
	void *callback_guest = nullptr;
	size_t callback_size = sizeof(callback_code);
	if (allocate(nullptr, &callback_guest, 0, &callback_size, 0x3000, 0x40))
		throw std::runtime_error("callback allocation failed");
	memcpy(callback_guest, callback_code, sizeof(callback_code));
	notification[0] = uintptr_t(callback_guest);
	notification[1] = callback_size;
	raven_ec_call(view.export_address("NotifyMemoryAlloc"), notification, teb);
	nested_code = uintptr_t(callback_guest);
	rip = uintptr_t(guest_code);
	memcpy(context + 0xf8, &rip, 8);
	memcpy(context + 0x98, &rsp, 8);
	expected_guest_end = rip + sizeof(code) - 2;
	exercise_nested = true;
	expected_resumed = 5008;
	guest_calls = 0;
	nested_calls = 0;
	guest_ok = false;
	if (!sigsetjmp(guest_return, 1)) {
		guest_active = true;
		raven_ec_call(view.export_address("BeginSimulation"), args, teb);
		std::abort();
	}
	asm volatile("mov x18,%0" ::"r"(previous_teb));
	guest_active = false;
	exercise_nested = false;
	if (!guest_ok || nested_calls != 1)
		throw std::runtime_error("nested callback result mismatch");
	std::cout
		<< "PASS: nested harness-to-guest callback argument=41 result=124 outer_resumed_result=5008\n"
		<< std::flush;
	uint8_t x87_code[]{0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xeb, 0xd9, 0xc9, 0xd8, 0xd1, 0xb8, 8, 1, 0, 0,
		0x0f, 0x05, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8b, 0, 0xde, 0xc1, 0xdd, 0x1c, 0x24,
		0x4c, 0x8b, 0x14, 0x24, 0xb8, 9, 1, 0, 0, 0x0f, 0x05, 0xeb, 0xfe};
	memcpy(x87_code + 19, &bad_value, 8);
	void *x87_guest = nullptr;
	size_t x87_size = sizeof(x87_code);
	if (allocate(nullptr, &x87_guest, 0, &x87_size, 0x3000, 0x40))
		throw std::runtime_error("x87 guest allocation failed");
	memcpy(x87_guest, x87_code, sizeof(x87_code));
	notification[0] = uintptr_t(x87_guest);
	notification[1] = x87_size;
	raven_ec_call(view.export_address("NotifyMemoryAlloc"), notification, teb);
	rip = uintptr_t(x87_guest);
	memcpy(context + 0xf8, &rip, 8);
	memcpy(context + 0x98, &rsp, 8);
	expected_fault_rip = rip + 27;
	expected_fault_address = bad_value;
	expected_guest_end = rip + sizeof(x87_code) - 2;
	x87_test = true;
	x87_dispatches = 0;
	faults_repaired = 0;
	guest_ok = false;
	if (!sigsetjmp(guest_return, 1)) {
		guest_active = true;
		raven_ec_call(view.export_address("BeginSimulation"), args, teb);
		std::abort();
	}
	asm volatile("mov x18,%0" ::"r"(previous_teb));
	guest_active = false;
	x87_test = false;
	if (!guest_ok)
		throw std::runtime_error("occupied x87 dispatch/fault round trip failed");
	std::cout << "PASS: occupied x87 dispatch and AV TOP/condition/tag/result preserved\n"
				<< std::flush;

	auto install_guest = [&](const std::vector<uint8_t> &blob) {
		void *base = nullptr;
		size_t size = blob.size();
		if (allocate(nullptr, &base, 0, &size, 0x3000, 0x40))
			throw std::runtime_error("benchmark allocation failed");
		memcpy(base, blob.data(), blob.size());
		notification[0] = uintptr_t(base);
		notification[1] = size;
		raven_ec_call(view.export_address("NotifyMemoryAlloc"), notification, teb);
		return uintptr_t(base);
	};
	auto integer_code = install_guest(RavenBench::kernel(false, 10000000));
	auto floating_code = install_guest(RavenBench::kernel(true, 10000000));
	auto roundtrip_code = install_guest(RavenBench::roundtrip(expected_timed_calls));
	uint64_t integer_reference = 7;
	double reference = 0.5;
	for (unsigned i = 0; i < 10000000; ++i) {
		integer_reference = integer_reference * 1664525 + 1013904223;
		reference = reference * 1.0000001 + 0.25;
	}
	if (integer_reference != integer_expected)
		throw std::runtime_error("integer reference mismatch");
	memcpy(&float_expected_bits, &reference, 8);
	auto ns = []() {
		timespec t{};
		clock_gettime(CLOCK_MONOTONIC_RAW, &t);
		return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
	};
	auto run_timed = [&](uintptr_t address, size_t terminal, bool floating) {
		memcpy(context + 0xf8, &address, 8);
		memcpy(context + 0x98, &rsp, 8);
		expected_guest_end = address + terminal;
		if (floating) {
			double values[]{0.5, 1.0000001, 0.25};
			for (unsigned i = 0; i < 3; ++i) {
				memset(context + 0x1a0 + i * 16, 0, 16);
				memcpy(context + 0x1a0 + i * 16, &values[i], 8);
			}
		}
		guest_ok = false;
		timed_calls = 0;
		auto begin = ns();
		if (!sigsetjmp(guest_return, 1)) {
			guest_active = true;
			raven_ec_call(view.export_address("BeginSimulation"), args, teb);
			std::abort();
		}
		asm volatile("mov x18,%0" ::"r"(previous_teb));
		guest_active = false;
		auto elapsed = ns() - begin;
		if (!guest_ok)
			throw std::runtime_error("benchmark result verification failed");
		return elapsed;
	};
	for (unsigned sample = 0; sample < 5; ++sample) {
		auto integer_ns = run_timed(integer_code, 25, false);
		auto float_ns = run_timed(floating_code, 25, true);
		std::cout << "BENCH sample=" << sample << " iterations=10000000 integer_ns=" << integer_ns
					<< " float_ns=" << float_ns << " integer_result=" << integer_expected
					<< " float_result=" << std::setprecision(17) << reference << "\n"
					<< std::flush;
	}
	for (unsigned sample = 0; sample < 5; ++sample) {
		auto elapsed = run_timed(roundtrip_code, 32, false);
		std::cout << "ROUNDTRIP sample=" << sample << " calls=" << expected_timed_calls
					<< " elapsed_ns=" << elapsed
					<< " ns_per_call=" << double(elapsed) / expected_timed_calls << "\n"
					<< std::flush;
	}
}
uint8_t *gap_base = nullptr;
volatile sig_atomic_t first_faults = 0, second_faults = 0;
void gap_fault(int, siginfo_t *info, void *) {
	auto offset = uintptr_t(info->si_addr) - uintptr_t(gap_base);
	if (offset < 4096)
		first_faults = first_faults + 1;
	else if (offset < 8192)
		second_faults = second_faults + 1;
	else
		_exit(2);
	if (mprotect(gap_base, second_faults ? 16384 : 4096, PROT_READ | PROT_WRITE))
		_exit(2);
}
int tier0_second_fault() {
	if (getpagesize() != 16384)
		return 2;
	gap_base =
		static_cast<uint8_t *>(mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
	if (gap_base == MAP_FAILED)
		return 2;
	struct sigaction action{};
	action.sa_sigaction = gap_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, nullptr) || sigaction(SIGSEGV, &action, nullptr))
		return 2;
	volatile uint8_t *memory = gap_base;
	auto first = memory[0], second = memory[4096];
	(void)first;
	(void)second;
	munmap(gap_base, 16384);
	if (first_faults == 1 && !second_faults) {
		std::cout
			<< "XFAIL: first_faults=1 second_faults=0; second 4K page was exposed by first repair\n";
		return 1;
	}
	std::cout << "XPASS: second logical page faulted independently\n";
	return 0;
}
void self_test() {
	assert(page_size() == 16384);
	alignas(16) uint8_t rejected_context[0x4d0]{};
	expected_fault_rip = 0x1234;
	expected_fault_address = 0x4567;
	faults_repaired = 0;
	for (unsigned invalid_case = 0; invalid_case < 3; ++invalid_case) {
		RavenFault::Record record{};
		record.code = 0xc0000005;
		record.count = 2;
		record.address = reinterpret_cast<void *>(expected_fault_rip);
		record.information[1] = expected_fault_address;
		uint64_t rip = expected_fault_rip;
		if (invalid_case == 0)
			record.information[0] = 1;
		if (invalid_case == 1) {
			++rip;
			record.address = reinterpret_cast<void *>(rip);
		}
		if (invalid_case == 2)
			++record.information[1];
		memcpy(rejected_context + 0xf8, &rip, 8);
		std::array<uint8_t, 0x4d0> before{};
		memcpy(before.data(), rejected_context, sizeof(rejected_context));
		assert(exception_callback(&record, rejected_context, nullptr) == RavenNT::not_supported);
		assert(
			!memcmp(before.data(), rejected_context, sizeof(rejected_context)) && !faults_repaired);
	}
	expected_fault_rip = expected_fault_address = 0;

	char formatted[64];
	uint64_t va[3]{42, uintptr_t("bird"), 0x3ff4000000000000ull};
	assert(RavenFormat::output(formatted, 64, "%d %s %.2f", va) == 12 &&
		!strcmp(formatted, "42 bird 1.25"));
	int scanned = 0;
	char scanned_text[16]{};
	assert(RavenFormat::scan("42 bird", "%d %15s", uintptr_t(&scanned), uintptr_t(scanned_text),
				nullptr, 0) == 2);
	assert(scanned == 42 && !strcmp(scanned_text, "bird"));
	uint64_t srw = 0, condition = 0;
	RavenSync::init_rw(&srw);
	RavenSync::init_cv(&condition);
	bool ready = false;
	std::atomic<bool> started{false};
	std::thread worker([&] {
		RavenSync::acquire_ex(&srw);
		started = true;
		while (!ready) {
			int64_t timeout = -10000000;
			auto result = RavenSync::sleep_cv(&condition, &srw, &timeout, 0);
			assert(result == 0 || result == 0x102);
		}
		RavenSync::release_ex(&srw);
	});
	while (!started)
		std::this_thread::yield();
	RavenSync::acquire_ex(&srw);
	ready = true;
	RavenSync::wake_cv(&condition);
	RavenSync::release_ex(&srw);
	worker.join();
	uint64_t address_value = 1, compare_value = 1;
	int64_t immediate = 0;
	assert(RavenSync::wait_address(&address_value, &compare_value, 8, &immediate) == 0x102);
	compare_value = 2;
	assert(RavenSync::wait_address(&address_value, &compare_value, 8, nullptr) == 0);
	void *reservation = nullptr;
	size_t reserved_size = 32768;
	assert(allocate(nullptr, &reservation, 0, &reserved_size, 0x2000, 4) == 0);
	auto *second = static_cast<char *>(reservation) + 16384;
	void *commit = second;
	size_t committed = 1;
	assert(allocate(nullptr, &commit, 0, &committed, 0x1000, 4) == 0 && committed == 16384);
	memset(commit, 0x6b, committed);
	size_t decommitted = committed;
	assert(release(nullptr, &commit, &decommitted, 0x4000) == 0);
	assert(allocate(nullptr, &commit, 0, &committed, 0x1000, 4) == 0);
	assert(*static_cast<char *>(commit) == 0);
	size_t zero = 0;
	assert(release(nullptr, &reservation, &zero, 0x8000) == 0);
	RavenVM::Extended ec{5, 0x40};
	void *native_code = nullptr;
	size_t native_size = 16;
	assert(RavenVM::allocate_ex(reinterpret_cast<void *>(-1), &native_code, &native_size, 0x3000,
				0x40, &ec, 1) == 0);
	uint32_t ret_instruction = 0xd65f03c0;
	NativeJit::write(native_code, &ret_instruction, 4);
	reinterpret_cast<void (*)()>(native_code)();
	uint32_t previous_protection = 0;
	size_t executable_size = native_size;
	assert(protect(nullptr, &native_code, &executable_size, 0x20, &previous_protection) == 0 &&
		previous_protection == 0x40);
	reinterpret_cast<void (*)()>(native_code)();
	assert(RavenVM::prot(0x20, false) == PROT_READ &&
		RavenVM::prot(0x20, true) == (PROT_READ | PROT_EXEC));
	auto write_mode = NativeJit::protect(0);
	previous_protection = 0xdead;
	assert(protect(nullptr, &native_code, &executable_size, 0x20, &previous_protection) ==
			RavenVM::unsupported &&
		previous_protection == 0xdead);
	NativeJit::protect(write_mode);
	zero = 0;
	assert(release(nullptr, &native_code, &zero, 0x8000) == 0);
	char temporary[] = "/tmp/raven-modhost-io-XXXXXX";
	int fd = mkstemp(temporary);
	assert(fd >= 0);
	close(fd);
	auto path = RavenNT::decode(temporary, strlen(temporary));
	RavenNT::String name{};
	RavenNT::init_unicode(&name, path.c_str());
	RavenNT::ObjectAttributes object{48, 0, nullptr, &name, 0, 0, nullptr, nullptr};
	RavenNT::IoStatus io{};
	void *file = nullptr;
	assert(
		RavenNT::create_file(&file, 0xc0000000, &object, &io, nullptr, 0, 7, 1, 0x20, 0, 0) == 0);
	int64_t position = 0;
	assert(
		RavenNT::write_file(file, nullptr, nullptr, nullptr, &io, "bird", 4, &position, 0) == 0 &&
		io.information == 4);
	char readback[4]{};
	assert(
		RavenNT::read_file(file, nullptr, nullptr, nullptr, &io, readback, 4, &position, 0) == 0 &&
		!memcmp(readback, "bird", 4));
	void *section = nullptr;
	assert(RavenNT::create_section(&section, 0, nullptr, nullptr, 2, 0x8000000, file) == 0);
	void *view = nullptr;
	size_t view_size = 0;
	assert(RavenNT::map_section(section, reinterpret_cast<void *>(-1), &view, 0, 0, nullptr,
				&view_size, 1, 0, 2) == 0 &&
		view_size == 4 && !memcmp(view, "bird", 4));
	assert(RavenNT::unmap_section(reinterpret_cast<void *>(-1), view) == 0);
	assert(RavenNT::close_handle(section) == 0);
	auto shared_views = [&](void *section_handle) {
		void *a = nullptr, *b = nullptr, *copy = nullptr;
		size_t n = 0;
		assert(!RavenNT::map_section(
			section_handle, reinterpret_cast<void *>(-1), &a, 0, 0, nullptr, &n, 1, 0, 4));
		n = 0;
		assert(!RavenNT::map_section(
			section_handle, reinterpret_cast<void *>(-1), &b, 0, 0, nullptr, &n, 1, 0, 4));
		n = 0;
		assert(!RavenNT::map_section(
			section_handle, reinterpret_cast<void *>(-1), &copy, 0, 0, nullptr, &n, 1, 0, 8));
		static_cast<char *>(a)[0] = 'B';
		assert(static_cast<char *>(b)[0] == 'B');
		static_cast<char *>(copy)[0] = 'X';
		assert(static_cast<char *>(a)[0] == 'B' && static_cast<char *>(b)[0] == 'B');
		for (auto *p : {a, b, copy})
			assert(!RavenNT::unmap_section(reinterpret_cast<void *>(-1), p));
	};
	assert(!RavenNT::create_section(&section, 0, nullptr, nullptr, 4, 0x8000000, file));
	shared_views(section);
	assert(!RavenNT::close_handle(section));
	char visible = 0;
	assert(!RavenNT::read_file(file, nullptr, nullptr, nullptr, &io, &visible, 1, &position, 0) &&
		visible == 'B');
	int64_t anonymous_size = 16384;
	assert(!RavenNT::create_section(&section, 0, nullptr, &anonymous_size, 4, 0x8000000, nullptr));
	shared_views(section);
	assert(!RavenNT::close_handle(section));
	assert(RavenNT::close_handle(file) == 0);
	unlink(temporary);
	alignas(16) uint8_t captured[0x4d0]{};
	uint64_t capture_args[8]{uintptr_t(captured)};
	uint64_t capture_teb[1536]{};
	raven_ec_call(
		reinterpret_cast<void *>(&RavenNT::raven_capture_ec_context), capture_args, capture_teb);
	assert(*reinterpret_cast<uint64_t *>(captured + 0xf8) &&
		*reinterpret_cast<uint64_t *>(captured + 0x98));
	std::array<uint8_t, 512> unwind_image{};
	PEView fixture_view{Bytes{unwind_image.data(), unwind_image.size()}, 0};
	auto *previous_image = RavenNT::active_image;
	RavenNT::active_image = &fixture_view;
	uint32_t unwind_header = 16 | (1u << 21) | (1u << 27);
	memcpy(unwind_image.data() + 128, &unwind_header, 4);
	uint8_t codes[4]{0x40, 1, 0xe4, 0xe3};
	memcpy(unwind_image.data() + 132, codes, 4);
	uint64_t unwind_stack[2]{0xabc, 0xdef};
	RavenNT::UnwindState state{captured};
	state.set(31, uintptr_t(unwind_stack));
	RavenNT::RuntimeFunction function{64, 128};
	uint64_t establisher = 0;
	void *handler_data = nullptr;
	assert(!RavenNT::virtual_unwind(0, uintptr_t(unwind_image.data()),
		uintptr_t(unwind_image.data()) + 96, &function, captured, &handler_data, &establisher,
		nullptr));
	assert(state.get(29) == 0xabc && state.get(30) == 0xdef &&
		state.get(31) == uintptr_t(unwind_stack) + 16);
	RavenNT::active_image = previous_image;
	RavenNT::ProcessEnvironment process;
	RavenNT::ThreadEnvironment thread(process);
	uint32_t initial_tls = 0x12345678;
	thread.install_tls(&initial_tls, 4, 12);
	assert(*static_cast<uint32_t *>(thread.tls_vector[0]) == initial_tls);
	assert(thread.static_tls[15] == 0 && thread.cpu.context == thread.context.data());
	assert(*reinterpret_cast<void **>(thread.teb.data() + 0x1788) == &thread.cpu);
	assert(cpu_area(thread.teb.data()) == &thread.cpu);
	RavenNT::active_process = &process;
	RavenNT::active_thread = &thread;
	RavenNT::pseudo_handle = RavenNT::current_handle;
	auto worker_entry = +[](void *p) -> uint64_t {
		require_native_x18();
		stack_t stack{};
		assert(!sigaltstack(nullptr, &stack) && !(stack.ss_flags & SS_DISABLE));
		return *static_cast<uint64_t *>(p) + 1;
	};
	NativeJit worker_jit;
	auto *native_worker = native_adapter(worker_jit, reinterpret_cast<void *>(worker_entry));
	uint64_t worker_input = 41;
	void *worker_handle = nullptr;
	assert(RavenNT::create_thread(&worker_handle, 0x1fffff, nullptr, reinterpret_cast<void *>(-1),
				native_worker, &worker_input, 0x10, 0, 0, 0, 0) == 0);
	std::weak_ptr<RavenNT::ThreadControl> normal_worker =
		RavenNT::get_handle(worker_handle)->control;
	assert(RavenNT::wait_one(worker_handle, 0, nullptr) == 0);
	assert(RavenNT::get_handle(worker_handle)->control->exit_status == 42);
	uint32_t object_info[14]{};
	assert(
		RavenNT::query_object(worker_handle, 0, object_info, sizeof(object_info), nullptr) == 0 &&
		object_info[1] == 0x1fffff);
	assert(RavenNT::close_handle(worker_handle) == 0);
	auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!normal_worker.expired() && std::chrono::steady_clock::now() < cleanup_deadline)
		std::this_thread::yield();
	assert(normal_worker.expired());
	RavenNT::String registry_name{};
	RavenNT::init_unicode(
		&registry_name, u"\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0");
	RavenNT::ObjectAttributes registry_object{
		48, 0, nullptr, &registry_name, 0, 0, nullptr, nullptr};
	void *registry_key = nullptr;
	assert(RavenNT::open_key(&registry_key, 0, &registry_object, 0) == 0);
	RavenNT::String value_name{};
	RavenNT::init_unicode(&value_name, u"CP 4030");
	uint8_t registry_value[20]{};
	assert(RavenNT::query_value(registry_key, &value_name, 2, registry_value, 20, nullptr) == 0);
	assert(RavenNT::close_handle(registry_key) == 0);
	RavenNT::import_lookup = resolve;
	import_jit.each_region([&](void *p, size_t n) { process.mark_ec(uintptr_t(p), n); });
	auto adapters_before = adapter_cache.size();
	RavenNT::String dynamic_name{};
	RavenNT::init_ansi(&dynamic_name, "strlen");
	void *dynamic = nullptr, *again = nullptr;
	assert(!RavenNT::procedure(&RavenNT::ntdll_token, &dynamic_name, 0, &dynamic));
	assert(!RavenNT::procedure(&RavenNT::ntdll_token, &dynamic_name, 0, &again) &&
		dynamic == again && RavenNT::is_ec(dynamic));
	assert(adapter_cache.size() == adapters_before + 1);
	uint64_t dynamic_args[8]{uintptr_t(dynamic), uintptr_t("dynamic")};
	assert(raven_ec_call(reinterpret_cast<void *>(&raven_checked_import), dynamic_args,
				thread.teb.data()) == 7);
	std::array<void *, 8> resolved{};
	std::vector<std::thread> resolvers;
	adapters_before = adapter_cache.size();
	NativeJit concurrent_jit;
	uint32_t ret = 0xd65f03c0;
	for (size_t i = 0; i < resolved.size(); ++i)
		resolvers.emplace_back([&, i] {
			require_native_x18();
			for (unsigned j = 0; j < 32; ++j) {
				resolved[i] = resolve("ntdll.dll", "sin");
				auto *code = concurrent_jit.immutable(&ret, 4);
				reinterpret_cast<void (*)()>(code)();
			}
		});
	for (auto &worker : resolvers)
		worker.join();
	for (auto *p : resolved)
		assert(p == resolved[0] && RavenNT::is_ec(p));
	assert(adapter_cache.size() == adapters_before + 1);
	size_t regions = 0;
	concurrent_jit.each_region([&](void *, size_t) { ++regions; });
	assert(regions == 8 * 32);
	auto terminating = +[](void *) -> uint64_t {
		require_native_x18();
		RavenNT::terminate_thread(reinterpret_cast<void *>(-2), 37);
		std::abort();
	};
	auto *termination_adapter = native_adapter(worker_jit, reinterpret_cast<void *>(terminating));
	auto environment_baseline = RavenNT::ThreadEnvironment::live_environments.load();
	size_t handle_baseline = RavenNT::handles.size();
	for (unsigned iteration = 0; iteration < 64; ++iteration) {
		void *handle = nullptr;
		assert(!RavenNT::create_thread(&handle, 0x1fffff, nullptr, reinterpret_cast<void *>(-1),
			termination_adapter, nullptr, 0x10, 0, 0, 0, 0));
		std::weak_ptr<RavenNT::ThreadControl> weak = RavenNT::get_handle(handle)->control;
		assert(!RavenNT::wait_one(handle, 0, nullptr));
		assert(RavenNT::get_handle(handle)->control->exit_status == 37);
		assert(!RavenNT::close_handle(handle));
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while ((!weak.expired() ||
					RavenNT::ThreadEnvironment::live_environments.load() != environment_baseline) &&
			std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		assert(weak.expired() &&
			RavenNT::ThreadEnvironment::live_environments.load() == environment_baseline &&
			RavenNT::handles.size() == handle_baseline);
	}
	RavenNT::active_process = nullptr;
	RavenNT::active_thread = nullptr;
	auto utf16 = RavenNT::decode("A\xf0\x9f\x90\xa6", 5);
	assert(utf16 == u"A\U0001f426" &&
		RavenNT::encode(utf16.data(), utf16.size()) == "A\xf0\x9f\x90\xa6");
	uint32_t count = 0;
	char16_t tiny[2]{};
	assert(RavenNT::to_unicode(tiny, 4, &count, "A\xf0\x9f\x90\xa6", 5) == int32_t(0x80000005) &&
		count == 2);
	uint32_t bits[2]{1, 0};
	RavenNT::Bitmap bitmap{64, 0, bits};
	assert(RavenNT::find_clear(&bitmap, 2, 1) == 1 && bits[0] == 7);
	RavenNT::clear_bits(&bitmap, 1, 2);
	assert(bits[0] == 1);
	char16_t text[] = u"4294967295!";
	char16_t *end = nullptr;
	assert(RavenCRT::wide_ulong(text, &end, 10) == UINT32_MAX && *end == '!');
	errno = 0;
	assert(RavenCRT::wide_long(text, &end, 10) == INT32_MAX && errno == ERANGE);
	assert(RavenCRT::length(u"\U0001f426") == 2);
	assert(RavenCRT::compare(u"bird", u"birds") < 0);
	char *last = nullptr;
	assert(RavenCRT::ansi_ulong("-1!", &last, 10) == UINT32_MAX && *last == '!');
	uint64_t args[8]{reinterpret_cast<uintptr_t>(text)};
	uint64_t teb[1536]{};
	auto *length_adapter = resolve("ntdll.dll", "wcslen");
	assert(length_adapter && raven_ec_call(length_adapter, args, teb) == 11);
	void *p = nullptr;
	size_t n = 1;
	assert(allocate(nullptr, &p, 0, &n, 0x3000, 4) == 0 && n == page_size());
	memset(p, 0x5a, n);
	MemoryInfo info{};
	size_t written = 0;
	assert(query(nullptr, p, 0, &info, sizeof(info), &written) == 0 && written == 48 &&
		info.region_size == n);
	uint32_t old = 0;
	assert(protect(nullptr, &p, &n, 2, &old) == 0 && old == 4);
	assert(static_cast<uint8_t *>(p)[n - 1] == 0x5a);
	assert(protect(nullptr, &p, &n, 4, &old) == 0 && old == 2);
	n = 0;
	assert(release(nullptr, &p, &n, 0x8000) == 0 && p == nullptr);
	auto *heap = static_cast<uint8_t *>(heap_alloc(nullptr, 8, 64));
	assert(heap && heap[63] == 0);
	assert(heap_free(nullptr, 0, heap));
	int key = 0;
	assert(initialize_lock(&key) == 0);
	assert(enter(&key) == 0);
	assert(try_enter(&key));
	assert(leave(&key) == 0);
	assert(leave(&key) == 0);
	assert(delete_lock(&key) == 0);
	int64_t a, b, f;
	assert(qpc(&a, &f) == 1 && f > 0);
	assert(qpc(&b, nullptr) == 1 && b >= a);
	std::cout
		<< "PASS: negative fault repair, dynamic checked imports/concurrency, shared sections, RX JIT, worker termination\n";
	std::cout
		<< "PASS: native VM, heap, CRT, SRW/condition/address waits, QPC substrate (not PE execution)\n";
}
} // namespace
#ifndef RAVEN_FEX_EMBEDDED
int main(int argc, char **argv) {
	require_native_x18();
	if (argc == 2 && std::string(argv[1]) == "--tier0-second-fault")
		return tier0_second_fault();
	if (argc == 2 && std::string(argv[1]) == "--unsupported-unwind") {
		uint8_t context[0x4d0]{};
		RavenNT::UnwindState state{context};
		RavenNT::apply_ops(state, {{0xff, 0, 1}, {0xe4, 0, 1}}, 0);
		return 2;
	}
	if (argc == 2 && std::string(argv[1]) == "--self-test") {
		self_test();
		return 0;
	}
	if (argc == 3 && std::string(argv[1]) == "--execute")
		return map_module(argc - 1, argv + 1, resolve, inspect_image, execute_image);
	return map_module(argc, argv, resolve, inspect_image);
}

#endif
