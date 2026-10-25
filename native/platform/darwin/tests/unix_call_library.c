#include "unix_call_shared.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static int32_t probe(void *args)
{
	struct unix_call_probe *p = args;
	char local;
	p->native_stack = (uint64_t)&local;
	p->native_thread = (uint64_t)pthread_self();
	__asm__ volatile("movq %%gs:0, %0" : "=r"(p->native_gs));
	if (p->native_stack >= p->guest_stack_low && p->native_stack < p->guest_stack_high)
		return (int32_t)0xc0000001;
	if (!p->native_thread || p->native_gs != p->native_thread)
		return (int32_t)0xc0000001;
	// Exercise Darwin TLS and a call long enough for the host's scheduler to interrupt the native stack.
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		usleep(1000);
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec < 50000000);
	errno = EDOM;
	if (pthread_self() != (pthread_t)p->native_thread || errno != EDOM || getpid() <= 0)
		return (int32_t)0xc0000001;
	++p->calls;
	// Deliberately destroy Win64's nonvolatile vector registers (which are volatile in the Darwin ABI).
	__asm__ volatile("pxor %%xmm6, %%xmm6; pxor %%xmm7, %%xmm7; pxor %%xmm8, %%xmm8; pxor %%xmm9, %%xmm9;"
		"pxor %%xmm10, %%xmm10; pxor %%xmm11, %%xmm11; pxor %%xmm12, %%xmm12; pxor %%xmm13, %%xmm13;"
		"pxor %%xmm14, %%xmm14; pxor %%xmm15, %%xmm15" :::
		"xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
	return 0x1234;
}

__attribute__((visibility("default"))) int32_t (*__wine_unix_call_funcs[])(void *) = {probe};
