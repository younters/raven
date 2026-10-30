#pragma once

#include <algorithm>
#include <cerrno>
#include <sys/resource.h>
#include <sys/sysctl.h>

namespace raven_darwin {

// GUI launches commonly inherit 256 descriptors. Guest files, pipes, shared
// surfaces and IPC watches all consume host descriptors from the same budget.
// Change only our soft limit; descendants inherit it through spawn/exec.
inline int prepare_fd_budget()
{
	rlimit limits{};
	if (::getrlimit(RLIMIT_NOFILE, &limits)) return errno;
	constexpr rlim_t budget = 4096;
	if (limits.rlim_cur >= budget) return 0;
	rlim_t target = std::min(budget, limits.rlim_max);
	int kernel_max = 0;
	size_t size = sizeof(kernel_max);
	if (!::sysctlbyname("kern.maxfilesperproc", &kernel_max, &size, nullptr, 0) && kernel_max > 0)
		target = std::min(target, rlim_t(kernel_max));
	if (target <= limits.rlim_cur) return 0;
	limits.rlim_cur = target;
	// Failure leaves the inherited budget intact; do not raise the hard limit
	// or retry with an unlimited value when the host refuses this request.
	return ::setrlimit(RLIMIT_NOFILE, &limits) ? errno : 0;
}

}
