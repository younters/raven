#pragma once
#include <stdint.h>

struct unix_call_probe {
	uint64_t gate, handle;
	uint64_t guest_stack_low, guest_stack_high, guest_teb;
	uint64_t native_stack, native_thread, native_gs, calls;
	uint32_t preserved, error;
	uint64_t native_allocation;
};
