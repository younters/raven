// SPDX-License-Identifier: MIT
#pragma once
#include "unix_memory.h"
#include <setjmp.h>
#include <signal.h>

// Include in the engine translation unit owning both dispatch and its signal handler.
// The enclosing engine owns signal installation. It invokes this recovery only
// after its JIT/overcommit handlers decline a synchronous memory fault.
static __thread sigjmp_buf *raven_unix_recovery;
static inline int raven_unix_recover(int signal)
{
    if (raven_unix_recovery && (signal == SIGSEGV || signal == SIGBUS))
        siglongjmp(*raven_unix_recovery, 1);
    return 0;
}

static inline int32_t raven_unix_invoke(int32_t (*function)(void *), void *args,
    uint32_t code, const struct raven_unix_contract *contract, raven_unix_check_range check)
{
    sigjmp_buf recovery;
    sigjmp_buf *previous = raven_unix_recovery;
    if (sigsetjmp(recovery, 1)) {
        raven_unix_recovery = previous;
        return (int32_t)0xc0000005u;
    }
    raven_unix_recovery = &recovery;
    int32_t result = (int32_t)0xc0000005u;
    if (check((uintptr_t)args, contract->argument_sizes[code], 1) &&
        contract->validate(code, args, check)) result = function(args);
    raven_unix_recovery = previous;
    return result;
}
