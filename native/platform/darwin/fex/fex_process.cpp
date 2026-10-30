// SPDX-License-Identifier: MIT
#include "fex_process.h"
#define RAVEN_FEX_EMBEDDED
#include "modhost/native_host.cpp"
#include <pthread.h>
#include <mach/mach_vm.h>

namespace {
Mapping module_mapping{MAP_FAILED, 0};
bool module_claimed = false;
sigjmp_buf stop_frame;
struct StopPacket {
	raven::NativeStop kind;
	uint64_t value, rip;
	RavenFault::Record exception;
} last_stop{};
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
	auto &record = *static_cast<RavenFault::Record *>(opaque);
	last_stop = {raven::NativeStop::exception, record.code, packet_word(packet, 0xf8), record};
	siglongjmp(stop_frame, 1);
}
void initialize_process(Bytes bytes, size_t opt)
{
	initialize_image(bytes, opt, process_dispatch, process_exception);
}
}

namespace raven {
struct FexProcess::State {
	pthread_t owner = pthread_self();
	std::vector<Mapping> images;
	void *stack = MAP_FAILED, *gate = MAP_FAILED;
	static constexpr size_t stack_size = 1024 * 1024;
	std::unique_ptr<RavenFault::Scope> faults;
	bool active = false;
	State() = default;
	~State()
	{
		faults.reset();
		// Mapping is a simple scope owner; the vector entries are released here.
		if (stack != MAP_FAILED) munmap(stack, stack_size);
		if (gate != MAP_FAILED) munmap(gate, 16384);
	}
	void require_owner() const
	{
		if (!pthread_equal(owner, pthread_self()))
			throw std::runtime_error("FEX process used from another host thread");
	}
};
FexProcess::FexProcess(std::span<LoadedImage *const> images, const std::string &root)
	: state(std::make_unique<State>())
{
	if (module_claimed)
		throw std::runtime_error("FEX module supports one process lifetime per host");
	if (root.empty())
		throw std::runtime_error("FEX requires RAVEN_FEX_MODULE_ROOT");
	module_claimed = true;
	std::string path = root + "/xtajit64.dll";
	char program[] = "raven-fex";
	char *argv[]{program, path.data()};
	if (map_module(2, argv, resolve, inspect_image, initialize_process, &module_mapping))
		throw std::runtime_error("FEX module initialization failed");
	// Guest x86 code is data to the arm64 host. Keep it non-executable; only FEX's
	// separate native code cache uses MAP_JIT. Never replace an existing host mapping.
	state->images.reserve(images.size());
	for (auto *image : images) {
		auto n = (image->data().size() + 16383) & ~size_t(16383);
		mach_vm_address_t address = image->base();
		if (mach_vm_allocate(mach_task_self(), &address, n, VM_FLAGS_FIXED) != KERN_SUCCESS)
			throw std::runtime_error("FEX guest image address unavailable");
		auto *p = reinterpret_cast<void *>(address);
		state->images.emplace_back();
		state->images.back().base = p;
		state->images.back().size = n;
		memcpy(p, image->data().data(), image->data().size());
	}
	state->stack = mmap(nullptr, State::stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	state->gate = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (state->stack == MAP_FAILED || state->gate == MAP_FAILED)
		throw std::runtime_error("FEX guest stack/gate allocation failed");
	// mov r10,rax; mov eax,completion; syscall; ud2
	uint8_t gate[]{0x49, 0x89, 0xc2, 0xb8, 0xfe, 0xff, 0xff, 0x7f, 0x0f, 0x05, 0x0f, 0x0b};
	memcpy(state->gate, gate, sizeof(gate));
	for (const auto &mapping : state->images) {
		uint64_t args[8]{uintptr_t(mapping.base), mapping.size, 0x3000, 0x40, 1, 0};
		raven_ec_call(mapped_image->export_address("NotifyMemoryAlloc"), args, thread_environment->teb.data());
	}
	uint64_t args[8]{uintptr_t(state->gate), 16384, 0x3000, 0x40, 1, 0};
	raven_ec_call(mapped_image->export_address("NotifyMemoryAlloc"), args, thread_environment->teb.data());
	state->faults = std::make_unique<RavenFault::Scope>(mapped_image->export_address("RavenHandleFault"));
}
FexProcess::~FexProcess() = default;
NativeResult FexProcess::call(uint64_t address, std::span<const uint64_t> arguments)
{
	state->require_owner();
	if (state->active || arguments.size() > 4)
		throw std::runtime_error("FEX entry requires an idle frame and at most four arguments");
	auto *packet = thread_environment->context.data();
	// Preserve ThreadInit's segment selectors and floating-point defaults.
	memset(packet + 0x78, 0, 17 * 8);
	auto rsp = uintptr_t(state->stack) + State::stack_size - 40;
	auto gate = uintptr_t(state->gate);
	memcpy(reinterpret_cast<void *>(rsp), &gate, 8);
	memcpy(packet + 0x98, &rsp, 8);
	memcpy(packet + 0xf8, &address, 8);
	constexpr size_t offsets[]{0x80, 0x88, 0xb8, 0xc0};
	for (size_t i = 0; i < arguments.size(); ++i)
		memcpy(packet + offsets[i], &arguments[i], 8);
	state->active = true;
	return resume();
}
NativeResult FexProcess::resume()
{
	state->require_owner();
	if (!state->active)
		throw std::runtime_error("FEX has no stopped call to resume");
	uint64_t args[8]{};
	auto previous = NativeJit::protect(1);
	if (!sigsetjmp(stop_frame, 1)) {
		raven_ec_call(mapped_image->export_address("BeginSimulation"), args, thread_environment->teb.data());
		std::abort();
	}
	asm volatile("mov x18,xzr");
	NativeJit::protect(previous);
	if (std::getenv("RAVEN_FEX_TRACE"))
		std::cerr << "FEX stop rip=" << std::hex << last_stop.rip << " address=" << last_stop.exception.information[1] << std::dec << "\n";
	if (last_stop.kind == NativeStop::returned)
		state->active = false;
	NativeResult result{last_stop.kind, last_stop.value, last_stop.rip,
		last_stop.kind == NativeStop::exception ? SIGSEGV : 0};
	if (last_stop.kind == NativeStop::exception) {
		auto &record = last_stop.exception;
		result.exception_flags = record.flags;
		if (record.count > 15) throw std::runtime_error("FEX exception information exceeds packet");
		result.exception_information.assign(record.information, record.information + record.count);
		if (record.count > 1) result.fault_address = record.information[1];
	}
	return result;
}
NativeContext FexProcess::context() const
{
	state->require_owner();
	NativeContext result{};
	auto &r = result.registers;
	auto *p = thread_environment->context.data();
	unsigned long long *registers[]{&r.rax, &r.rcx, &r.rdx, &r.rbx, &r.rsp, &r.rbp, &r.rsi, &r.rdi,
		&r.r8, &r.r9, &r.r10, &r.r11, &r.r12, &r.r13, &r.r14, &r.r15, &r.rip};
	for (size_t i = 0; i < 17; ++i)
		*registers[i] = packet_word(p, 0x78 + i * 8);
	r.gs_base = uintptr_t(thread_environment->teb.data());
	r.eflags = *reinterpret_cast<uint32_t *>(p + 0x44);
	result.xstate.assign(p + 0x100, p + 0x300);
	return result;
}
void FexProcess::set_context(const NativeContext &value)
{
	state->require_owner();
	const auto &r = value.registers;
	if (r.fs_base || r.gs_base != uintptr_t(thread_environment->teb.data()))
		throw std::runtime_error("FEX external TEB/segment bases are not connected yet");
	if (value.xstate.size() != 512)
		throw std::runtime_error("FEX requires a 512-byte legacy floating-point packet");
	const uint64_t registers[]{r.rax, r.rcx, r.rdx, r.rbx, r.rsp, r.rbp, r.rsi, r.rdi,
		r.r8, r.r9, r.r10, r.r11, r.r12, r.r13, r.r14, r.r15, r.rip};
	auto *p = thread_environment->context.data();
	memcpy(p + 0x78, registers, sizeof(registers));
	uint32_t flags = uint32_t(r.eflags);
	memcpy(p + 0x44, &flags, 4);
	memcpy(p + 0x100, value.xstate.data(), 512);
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
