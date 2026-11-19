#pragma once
#include_next <poll.h>
#include <signal.h>
#include <time.h>

// ppoll with poll's millisecond resolution, rounding up so a short wait is never zero. A signal mask is not supported.
static inline int ppoll(struct pollfd *descriptors, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
	(void)mask;
	int milliseconds = -1;
	if (timeout) {
		const long long total = (long long)timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
		milliseconds = total > 0x7fffffff ? 0x7fffffff : (int)total;
	}
	return poll(descriptors, count, milliseconds);
}
