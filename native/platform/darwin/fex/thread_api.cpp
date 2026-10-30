// SPDX-License-Identifier: MIT
#include "thread_api.h"
#include "fex_process.h"
namespace raven {
void register_fex_thread_context(ApiRegistry &registry, NativeThreads &threads,
	std::function<std::shared_ptr<GuestThread>(uint64_t)> thread_from)
{
	if (!fex_execution_selected()) return;
	registry.add("SetThreadContext", 2, [thread_from, &threads](ApiCall &call) {
		if (call.pointer_width() != 8 || !call[1]) { call.fail(87); return; }
		auto thread = thread_from(call[0]);
		if (!thread || thread->state == GuestThread::State::done) { call.fail(6); return; }
		const bool self = thread->id == threads.current_id();
		// A running remote thread has no stable context. Windows callers suspend
		// first; don't fabricate a snapshot of concurrently changing JIT state.
		if (!self && !thread->suspend_count) { call.fail(87); return; }
		auto context = self ? call.process.context() : thread->snapshot.context;
		auto &r = context.registers;
		auto flags = call.load<uint32_t>(call[1] + 0x30);
		if ((flags & 0x100000) == 0 || (flags & ~0x100003u)) { call.fail(50); return; }
		if (flags & 1) {
			r.rsp = call.load<uint64_t>(call[1] + 0x98);
			r.rbp = call.load<uint64_t>(call[1] + 0xa0);
			r.rip = call.load<uint64_t>(call[1] + 0xf8);
			r.eflags = (call.load<uint32_t>(call[1] + 0x44) & 0x3f7fd7u) | 2;
		}
		if (flags & 2) {
			unsigned long long *registers[]{&r.rax, &r.rcx, &r.rdx, &r.rbx, &r.rsi, &r.rdi,
				&r.r8, &r.r9, &r.r10, &r.r11, &r.r12, &r.r13, &r.r14, &r.r15};
			const size_t offsets[]{0x78, 0x80, 0x88, 0x90, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0};
			for (size_t i = 0; i < 14; ++i) *registers[i] = call.load<uint64_t>(call[1] + offsets[i]);
		}
		if (self) call.process.set_context(context);
		else { thread->snapshot.context = std::move(context); thread->snapshot.context_in_task = false; }
		call.ret(1);
	});
}
}
