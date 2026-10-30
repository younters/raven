// SPDX-License-Identifier: MIT
#pragma once
#include "jit.h"
#include <array>
extern "C" uint64_t raven_ec_call(void *, const uint64_t *, void *teb);
extern "C" void raven_native_call();

inline void *native_adapter(NativeJit &jit, void *target, unsigned stack_args = 0) {
	if (stack_args > 8)
		throw std::runtime_error("too many native adapter arguments");
	// ldr x16, literal; mov x17, #stack_args; ldr x15, literal; br x15;
	// target and bridge literals. No compiler-generated EC classification.
	std::array<uint32_t, 8> code{
		0x58000090, 0xd2800011u | (stack_args << 5), 0x5800008f, 0xd61f01e0, 0, 0, 0, 0};
	uintptr_t bridge = reinterpret_cast<uintptr_t>(&raven_native_call);
	memcpy(code.data() + 4, &target, 8);
	memcpy(code.data() + 6, &bridge, 8);
	// These immutable veneers must remain executable while FEX opens a native
	// JIT write scope. MAP_JIT veneers would fault in that thread-wide mode.
	return jit.immutable(code.data(), sizeof(code));
}

inline uint64_t native_x18() {
	uint64_t value;
	asm volatile("mov %0,x18" : "=r"(value));
	return value;
}
inline void require_native_x18() {
	if (native_x18() != 0)
		__builtin_trap();
}
