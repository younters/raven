// SPDX-License-Identifier: MIT
#include "abi.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ucontext.h>
static void check(bool value) {
	if (!value) {
		fprintf(stderr, "ABI/JIT check failed\n");
		std::abort();
	}
}
static uint64_t sum(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
	uint64_t g, uint64_t h) {
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
extern "C" void raven_abi_fixture();
extern "C" void raven_abi_native_clobber();
extern "C" uint64_t raven_abi_verify_native(void *, const uint64_t *, void *);
extern "C" void raven_abi_spin();
extern "C" void raven_signal_entry(int, siginfo_t *, void *);
static uint64_t native18() {
	uint64_t v;
	asm volatile("mov %0,x18" : "=r"(v));
	return v;
}
static volatile sig_atomic_t signals = 0, bad_signal18 = 0;
static uint64_t signal_teb = 0;
extern "C" void raven_fault_handler(int, siginfo_t *, void *opaque) {
	auto *uc = static_cast<ucontext_t *>(opaque);
	if (native18() != 0 || uc->uc_mcontext->__ss.__x[18] != signal_teb)
		bad_signal18 = 1;
	signals = signals + 1;
}
static uint64_t sum16(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
	uint64_t g, uint64_t h, uint64_t i, uint64_t j, uint64_t k, uint64_t l, uint64_t m, uint64_t n,
	uint64_t o, uint64_t p) {
	check(native18() == 0);
	raven_abi_native_clobber();
	return sum(a, b, c, d, e, f, g, h) + 9 * i + 10 * j + 11 * k + 12 * l + 13 * m + 14 * n +
		15 * o + 16 * p;
}
int main() {
	check(getpagesize() == 16384);
	NativeJit jit;
	check(NativeJit::protect(2) == 0);
	auto adapter = native_adapter(jit, reinterpret_cast<void *>(&sum));
	uint64_t args[8]{1, 2, 3, 4, 5, 6, 7, 8};
	uint64_t teb[1536]{};
	check(raven_ec_call(adapter, args, teb) == 204);
	check(NativeJit::protect(0) == 1);
	check(raven_ec_call(adapter, args, teb) == 204);
	check(NativeJit::protect(0) == 0);
	check(NativeJit::protect(1) == 0);
	check(NativeJit::protect(1) == 1);
	auto adapter16 = native_adapter(jit, reinterpret_cast<void *>(&sum16), 8);
	args[0] = reinterpret_cast<uintptr_t>(adapter16);
	check(raven_abi_verify_native(reinterpret_cast<void *>(&raven_abi_fixture), args, teb) == 1496);
	check(native18() == 0);
	struct sigaction action{};
	action.sa_sigaction = raven_signal_entry;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	check(sigaction(SIGALRM, &action, nullptr) == 0);
	signal_teb = uintptr_t(teb);
	itimerval timer{{0, 1000}, {0, 1000}};
	check(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
	args[0] = 20000000;
	check(raven_ec_call(reinterpret_cast<void *>(&raven_abi_spin), args, teb) == 1);
	timer = {};
	check(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
	check(signals > 0 && !bad_signal18 && native18() == 0);
	puts(
		"PASS: MAP_JIT execution, nested write modes, sixteen arguments, d8-d15/q8-q15/x18 sentinels and native/signal x18");
}
