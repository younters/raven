// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
// Experimental v2: all callbacks are native arm64, non-variadic, <= 3 arguments.
// Context packets are the pinned FEX Windows ARM64/AMD64 contexts, NOT Darwin ucontext.
// No callback may throw across this boundary. Host retains table and backing storage.
struct RavenHostV2 {
	uint32_t size, version;
	uint64_t flags; // must be zero (Tier 0, software TSO)
	uint64_t guest32_base;
	const void *shared_data;
	uint64_t (*dispatch)(uint64_t syscall, void *amd64_context);
	int32_t (*unix_call)(uint64_t handle, uint32_t ordinal, void *args);
	int32_t (*exception)(void *record, void *amd64_context, void *arm64_context);
	void *(*cpu_area)(void *teb);
	void (*log)(const char *message);
	uint32_t (*jit_write_protect)(uint32_t mode);
	void *(*allocate_native_jit)(uint64_t size);
	void (*memory_event)(uint64_t address, uint64_t size, uint64_t kind);
	void *(*private_teb)(); // Preallocated per-host-worker TEB; never the guest TEB.
};

static_assert(sizeof(RavenHostV2) == 104, "FEX host ABI v2 size");
