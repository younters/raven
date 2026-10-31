// SPDX-License-Identifier: MIT
#pragma once
#include "context.h"
#include "signal_stack.h"
#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>
namespace RavenFault {
struct Record {
	uint32_t code, flags;
	void *chained, *address;
	uint32_t count, pad;
	uint64_t information[15];
};
struct alignas(16) NativeContext {
	uint32_t flags, cpsr;
	uint64_t x[31], sp, pc;
	__uint128_t v[32];
	uint32_t fpcr, fpsr;
	uint8_t debug[120];
};
static_assert(sizeof(NativeContext) == 0x390 && offsetof(NativeContext, v) == 0x110);
void *entry = nullptr;
// Fault dispatch must use the guest TEB of this host worker, including callbacks.
thread_local void *execution_teb = nullptr;

// Only active while emulating an alignment fault. A decoder memory access
// may fault; restart dispatch with the original guest PC and an AV record.
thread_local sigjmp_buf *alignment_recovery = nullptr;
thread_local NativeContext alignment_original;
thread_local uintptr_t alignment_fault_address;
thread_local uint64_t alignment_fault_write;
extern "C" void raven_signal_entry(int, siginfo_t *, void *);
extern "C" void raven_fault_handler(int signal, siginfo_t *info, void *opaque) {
	require_native_x18();
	if (alignment_recovery && (signal == SIGSEGV || signal == SIGBUS)) {
		alignment_fault_address = uintptr_t(info->si_addr);
		alignment_fault_write = (static_cast<ucontext_t *>(opaque)->uc_mcontext->__es.__esr >> 6) & 1;
		auto *recovery = alignment_recovery;
		alignment_recovery = nullptr;
		siglongjmp(*recovery, 1);
	}
	auto *uc = static_cast<ucontext_t *>(opaque);
	auto &ss = uc->uc_mcontext->__ss;
	auto &ns = uc->uc_mcontext->__ns;
	NativeContext native{};
	native.flags = 0x400007;
	native.cpsr = ss.__cpsr;
	for (unsigned i = 0; i < 29; ++i)
		native.x[i] = ss.__x[i];
	native.x[29] = arm_thread_state64_get_fp(ss);
	native.x[30] = arm_thread_state64_get_lr(ss);
	native.sp = arm_thread_state64_get_sp(ss);
	native.pc = arm_thread_state64_get_pc(ss);
	memcpy(native.v, ns.__v, sizeof(native.v));
	native.fpcr = ns.__fpcr;
	native.fpsr = ns.__fpsr;
	Record record{};
	record.code = signal == SIGBUS && (uc->uc_mcontext->__es.__esr & 0x3f) == 0x21 ? 0x80000002 : signal == SIGTRAP ? 0x80000003 : signal == SIGILL ? 0xc000001d : 0xc0000005;
	record.address = reinterpret_cast<void *>(native.pc);
	record.count = (signal == SIGSEGV || signal == SIGBUS) ? 2 : 0;
	record.information[0] = (uc->uc_mcontext->__es.__esr >> 6) & 1;
	record.information[1] = uintptr_t(info->si_addr);
	if (entry && RavenNT::active_thread) {
		uint64_t args[8]{uintptr_t(&record), uintptr_t(&native)};
		// FEX may backpatch acquire/release instructions while repairing an
		// unaligned access. Signal delivery enters with the guest's JIT RX mode.
		const auto previous_jit = NativeJit::protect(0);
		sigjmp_buf recovery;
		if (sigsetjmp(recovery, 1)) {
			native = alignment_original;
			record.code = 0xc0000005;
			record.information[0] = alignment_fault_write;
			record.information[1] = alignment_fault_address;
		} else if (record.code == 0x80000002) {
			alignment_original = native;
			alignment_recovery = &recovery;
		}
		auto status = uint32_t(raven_ec_call(entry, args, execution_teb ? execution_teb : RavenNT::active_thread->teb.data()));
		alignment_recovery = nullptr;
		NativeJit::protect(previous_jit);
		if (!status) {
			for (unsigned i = 0; i < 29; ++i)
				ss.__x[i] = native.x[i];
			arm_thread_state64_set_fp(ss, native.x[29]);
			arm_thread_state64_set_lr_fptr(ss, reinterpret_cast<void (*)()>(native.x[30]));
			arm_thread_state64_set_sp(ss, native.sp);
			arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void (*)()>(native.pc));
			ss.__cpsr = native.cpsr;
			memcpy(ns.__v, native.v, sizeof(native.v));
			ns.__fpcr = native.fpcr;
			ns.__fpsr = native.fpsr;
			return;
		}
	}
	if (getenv("RAVEN_FEX_TRACE")) {
		char detail[192];
		const int length = snprintf(detail, sizeof(detail), "FEX native signal=%d code=%d esr=%x pc=%llx instruction=%x address=%llx entry=%p jit=%d\n",
			signal, info->si_code, uc->uc_mcontext->__es.__esr, native.pc,
			*reinterpret_cast<const uint32_t *>(native.pc), record.information[1], entry, NativeJit::write_protected);
		write(2, detail, std::min<size_t>(length, sizeof(detail) - 1));
	}
	const char error[] = "unhandled FEX native or unsupported fault\n";
	write(2, error, sizeof(error) - 1);
	_exit(128 + signal);
}
struct Scope {
	AlternateSignalStack alternate;
	static constexpr int signals[]{SIGSEGV, SIGBUS, SIGTRAP, SIGILL};
	struct sigaction previous[4]{};
	explicit Scope(void *fault_entry) {
		entry = fault_entry;
		if (!entry)
			throw std::runtime_error("missing RavenHandleFault export");
		struct sigaction action{};
		action.sa_sigaction = raven_signal_entry;
		action.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&action.sa_mask);
		for (size_t i = 0; i < 4; ++i) {
			if (sigaction(signals[i], &action, &previous[i])) {
				while (i) { --i; sigaction(signals[i], &previous[i], nullptr); }
				entry = nullptr;
				throw std::runtime_error("sigaction failed");
			}
		}
	}
	~Scope() {
		for (size_t i = 0; i < 4; ++i) sigaction(signals[i], &previous[i], nullptr);
		entry = nullptr;
	}
};
} // namespace RavenFault
