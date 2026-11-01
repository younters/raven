// SPDX-License-Identifier: MIT
#pragma once
#include "unix_memory.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <stdlib.h>
#include <dlfcn.h>

// Include in the engine translation unit owning both dispatch and its signal handler.
// The enclosing engine owns signal installation. It invokes this recovery only
// after its JIT/overcommit handlers decline a synchronous memory fault.
static __thread sigjmp_buf *raven_unix_recovery;
#if defined(__x86_64__)
static __thread uintptr_t raven_unix_raise_pc;
#endif
static inline int raven_unix_explicit_fault(const siginfo_t *info, void *opaque)
{
    if (info->si_code == SI_USER || info->si_code == SI_QUEUE) return 1;
#if defined(__aarch64__)
    // Darwin pthread_kill/raise reports code 2, not SI_USER. The saved ESR is
    // SVC64 (rather than an instruction/data abort) and has no fault address.
    const ucontext_t *uc = (const ucontext_t *)opaque;
    return !info->si_addr && (uc->uc_mcontext->__es.__esr >> 26) == 0x15;
#else
    const ucontext_t *uc = (const ucontext_t *)opaque;
    // Rosetta leaves trapno from the previous guest stop in synthetic signals.
    // Identify the pthread_kill syscall return PC cached outside the handler.
    return !info->si_addr && raven_unix_raise_pc && uc->uc_mcontext->__ss.__rip == raven_unix_raise_pc;
#endif
}

// Return through sigreturn before leaving the faulting stack. Darwin then clears
// SS_ONSTACK and restores the signal mask, including for a second stack fault.
static __attribute__((noreturn)) void raven_unix_landing(void)
{
    siglongjmp(*raven_unix_recovery, 1);
}
static inline int raven_unix_recover(int signal, void *opaque)
{
    if (!raven_unix_recovery || (signal != SIGSEGV && signal != SIGBUS)) return 0;
    ucontext_t *uc = (ucontext_t *)opaque;
#if defined(__aarch64__)
    arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, raven_unix_landing);
#else
    uc->uc_mcontext->__ss.__rip = (uintptr_t)raven_unix_landing;
    uc->uc_mcontext->__ss.__rsp = (uc->uc_mcontext->__ss.__rsp & ~(uintptr_t)15) - 8;
#endif
    return 1;
}

static inline int32_t raven_unix_invoke(int32_t (*function)(void *), void *args,
    uint32_t code, const struct raven_unix_contract *contract, raven_unix_check_range check)
{
#if defined(__x86_64__)
    if (!raven_unix_raise_pc) {
        const unsigned char *stub = (const unsigned char *)dlsym(RTLD_DEFAULT,"__pthread_kill");
        if (stub) for (unsigned i=0;i<32;++i)
            if (stub[i]==0x0f && stub[i+1]==0x05) { raven_unix_raise_pc=(uintptr_t)(stub+i+2); break; }
    }
#endif
    sigjmp_buf recovery;
    sigjmp_buf *previous = raven_unix_recovery;
    if (sigsetjmp(recovery, 0)) {
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
