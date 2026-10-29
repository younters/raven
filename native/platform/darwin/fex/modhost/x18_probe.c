// SPDX-License-Identifier: MIT
// Standalone prerequisite probe, not a PE loader or a FEX execution test.
#include <dlfcn.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/ucontext.h>

static const uint64_t marker = UINT64_C(0x123456789abcdef0);
static volatile sig_atomic_t signals, bad_context, bad_live;
static inline uint64_t get18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static inline void put18(uint64_t v) { __asm__ volatile("mov x18, %0" :: "r"(v)); }
static void handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)info;
    ucontext_t *uc = ctx;
    if (uc->uc_mcontext->__ss.__x[18] != marker) ++bad_context;
    if (get18() != marker) ++bad_live;
    ++signals;
}
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    int enable = argc == 2 && !strcmp(argv[1], "--enable");
    _Bool (*query)(void) = (_Bool (*)(void))dlsym(RTLD_DEFAULT, "os_custom_x18_abi_enabled");
    void (*toggle)(_Bool) = (void (*)(_Bool))dlsym(RTLD_DEFAULT, "os_set_custom_x18_abi_enabled");
    printf("pid=%d page_size=%d toggle=%d query=%d mode=%s\n", getpid(), getpagesize(), !!toggle, !!query, enable ? "custom" : "default");
    if (enable) {
        if (!query || !toggle) return 2;
        printf("before_enable=%d\n", query());
        if (query()) return 2;
        // No libc/framework call in custom mode. Signal handler only accesses memory.
        struct sigaction sa = { .sa_sigaction = handler, .sa_flags = SA_SIGINFO };
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGALRM, &sa, NULL)) return 2;
        struct itimerval timer = { .it_interval = {0, 1000}, .it_value = {0, 1000} };
        if (setitimer(ITIMER_REAL, &timer, NULL)) return 2;
        toggle(1); // Unentitled processes may be terminated here; caller records wait status.
        put18(marker);
        unsigned bad_spin = 0;
        for (unsigned i = 0; i < 100000000; ++i) if (get18() != marker) ++bad_spin;
        uint64_t final = get18();
        toggle(0);
        struct itimerval off = {0}; setitimer(ITIMER_REAL, &off, NULL);
        printf("custom final=%016" PRIx64 " spin_bad=%u signals=%d context_bad=%d live_bad=%d\n", final, bad_spin, signals, bad_context, bad_live);
        return final != marker || bad_spin || !signals || bad_context;
    }
    uint64_t original = get18();
    put18(marker);
    unsigned bad_spin = 0;
    for (unsigned i = 0; i < 10000000; ++i) if (get18() != marker) ++bad_spin;
    uint64_t spin = get18(); put18(original);
    unsigned bad_sleep = 0;
    for (unsigned i = 0; i < 100; ++i) {
        struct timespec delay = {0, 1000000};
        put18(marker); nanosleep(&delay, NULL);
        if (get18() != marker) ++bad_sleep;
        put18(original);
    }
    struct sigaction sa = { .sa_sigaction = handler, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL)) return 2;
    struct itimerval timer = { .it_interval = {0, 1000}, .it_value = {0, 1000} };
    if (setitimer(ITIMER_REAL, &timer, NULL)) return 2;
    put18(marker);
    for (unsigned i = 0; i < 100000000; ++i) __asm__ volatile("" ::: "memory");
    uint64_t after_signals = get18(); put18(original);
    struct itimerval off = {0}; setitimer(ITIMER_REAL, &off, NULL);
    printf("spin=%016" PRIx64 " spin_bad=%u sleep_bad=%u/100 signals=%d context_bad=%d live_bad=%d after_signals=%016" PRIx64 "\n", spin, bad_spin, bad_sleep, signals, bad_context, bad_live, after_signals);
    return bad_spin || spin != marker || bad_sleep || !signals || bad_context || after_signals != marker;
}
