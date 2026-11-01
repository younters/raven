#include "../unix_guard.h"
#include "../dxmt/checked_memory.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
static unsigned faults, after_raise;
static void handler(int signal, siginfo_t *info, void *context) {
    (void)info;
    ++faults;
    if (raven_unix_recovery) assert(raven_unix_explicit_fault(info,context));
    if (raven_unix_recover(signal, context)) return;
    _Exit(90);
}
static int valid(uint32_t code, void *args, raven_unix_check_range check) { return 1; }
static void set_check(raven_unix_check_range check) {}
static const uint32_t sizes[]={0};
static const struct raven_unix_contract contract={1,1,sizes,valid,set_check};
static int32_t good(void *p) { return 17; }
static int32_t fail(void *p) { raise(SIGSEGV); ++after_raise; return 42; }
static int32_t checked(void *p) { raven_unix_checked((void *)1, 1, 1, 0); ++after_raise; return 42; }
static int32_t nested(void *p) {
    assert((uint32_t)raven_unix_invoke(fail,0,0,&contract,raven_unix_mapped_range)==0xc0000005u);
    assert(raven_unix_invoke(good,0,0,&contract,raven_unix_mapped_range)==17);
    return fail(0);
}
__attribute__((noinline,optnone)) static void overflow(unsigned n) {
    volatile char bytes[8192]; bytes[0]=n;
    overflow(n+1); bytes[1]=bytes[0];
}
int main(void) {
    stack_t stack={0}; stack.ss_size=SIGSTKSZ*4; stack.ss_sp=malloc(stack.ss_size);
    assert(!sigaltstack(&stack,0));
    struct sigaction action={0}; action.sa_sigaction=handler; action.sa_flags=SA_SIGINFO|SA_ONSTACK;
    sigaction(SIGSEGV,&action,0); sigaction(SIGBUS,&action,0);
    for (unsigned i=0;i<100;i++) {
        assert((uint32_t)raven_unix_invoke(nested,0,0,&contract,raven_unix_mapped_range)==0xc0000005u);
        assert((uint32_t)raven_unix_invoke(checked,0,0,&contract,raven_unix_mapped_range)==0xc0000005u);
        stack_t current; assert(!sigaltstack(0,&current)); assert(!(current.ss_flags&SS_ONSTACK));
    }
    assert(!after_raise && faults==300);
    assert(raven_unix_invoke(good,0,0,&contract,raven_unix_mapped_range)==17);
    // Same thread, after recovery: a real stack overflow must still reach altstack.
    pid_t child=fork(); assert(child>=0);
    if (!child) overflow(0);
    int status; waitpid(child,&status,0); assert(WIFEXITED(status)&&WEXITSTATUS(status)==90);
    return 0;
}
