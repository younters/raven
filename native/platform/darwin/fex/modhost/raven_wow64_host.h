// SPDX-License-Identifier: MIT
#pragma once
#include "raven_host.h"
#include <cstddef>
struct RavenWow64HostV1 {
	uint32_t size, version;
	RavenHostV2 common;
	uint64_t (*teb32)(void *native_teb);
};
// Guest context storage is the pinned 716-byte WOW64_CONTEXT. The common
// table's cpu_area, dispatch and exception callbacks designate that packet.
static_assert(sizeof(RavenWow64HostV1) == 120);
static_assert(offsetof(RavenWow64HostV1, common) == 8);
static_assert(offsetof(RavenWow64HostV1, teb32) == 112);
