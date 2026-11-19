// prctl on macOS, as far as the runtime uses it. macOS has no child subreaper: a process cannot take in the orphans of its children, so
// the request fails and the runtime goes on without it, as on a Linux kernel that lacks the option.
#pragma once
#include <errno.h>

#define PR_SET_CHILD_SUBREAPER 36

static inline int prctl(int option, ...)
{
	(void)option;
	errno = EINVAL;
	return -1;
}
