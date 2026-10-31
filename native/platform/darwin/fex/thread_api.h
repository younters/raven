// SPDX-License-Identifier: MIT
#pragma once
#include "nt/api.h"
#include "nt/threads.h"
namespace raven {
bool validate_fex_thread_context(ApiCall &, const GuestThread &, uint32_t current);
void register_fex_thread_context(ApiRegistry &, NativeThreads &, std::function<std::shared_ptr<GuestThread>(uint64_t)>);
}
