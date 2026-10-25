#include "unix_call_shared.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static void *native_worker(void *args)
{
	(void)args;
	const pthread_t self = pthread_self();
	errno = EDOM;
	raise(SIGUSR1);
	return (void *)(uintptr_t)(pthread_self() == self && errno == EDOM);
}

static int32_t probe(void *args)
{
	struct unix_call_probe *p = args;
	if (!p->native_allocation)
		p->native_allocation = (uint64_t)strdup("native allocation");
	if (!p->calls) {
		pthread_t worker;
		void *result = NULL;
		if (pthread_create(&worker, NULL, native_worker, NULL) || pthread_join(worker, &result) || result != (void *)1)
			return (int32_t)0xc0000001;
	}
	// Both native calls must enter before either returns: a serialized gate fails this rendezvous.
	if (!p->calls && p->rendezvous) {
		uint32_t *arrivals = (uint32_t *)(uintptr_t)p->rendezvous;
		__atomic_add_fetch(arrivals, 1, __ATOMIC_ACQ_REL);
		unsigned waits = 0;
		while (__atomic_load_n(arrivals, __ATOMIC_ACQUIRE) != 2) {
			if (++waits == 2000)
				return (int32_t)0xc0000001;
			usleep(1000);
		}
	}
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
		errno = EDOM;
		usleep(1000);
		if (pthread_self() != (pthread_t)p->native_thread || (errno != EDOM && errno != EINTR))
			return (int32_t)0xc0000001;
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
