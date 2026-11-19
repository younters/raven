#include "fd_budget.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libproc.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static void check(bool ok, const char *message)
{
	if (!ok) throw std::runtime_error(message);
}

static int fd_count()
{
	proc_fdinfo entries[8192];
	const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, entries, sizeof(entries));
	check(bytes > 0 && bytes < int(sizeof(entries)), "cannot count descriptors");
	return bytes / int(sizeof(proc_fdinfo));
}

// Keep 300 ordinary files and 64 pipe pairs alive concurrently, then release
// them.
static bool workload()
{
	std::vector<int> descriptors;
	bool ok = true;
	int file_error = 0, pipe_error = 0;
	for (int i = 0; i < 300; ++i) {
		const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd < 0) { file_error = errno; ok = false; break; }
		descriptors.push_back(fd);
	}
	for (int i = 0; i < 64; ++i) {
		int pair[2];
		if (pipe(pair)) { pipe_error = errno; ok = false; break; }
		descriptors.push_back(pair[0]);
		descriptors.push_back(pair[1]);
	}
	std::printf("files_errno=%d pipe_errno=%d fd_live=%d\n", file_error, pipe_error, fd_count());
	for (int fd : descriptors) close(fd);
	return ok;
}

int main(int argc, char **argv)
{
	try {
		if (argc == 4 && !std::strcmp(argv[1], "--inherited")) {
			rlimit limits{};
			check(!getrlimit(RLIMIT_NOFILE, &limits), "get inherited limits");
			check(limits.rlim_cur == std::strtoull(argv[2], nullptr, 10) &&
				limits.rlim_max == std::strtoull(argv[3], nullptr, 10), "exec did not inherit limits");
			return 0;
		}
		rlimit original{};
		check(!getrlimit(RLIMIT_NOFILE, &original), "get original limits");
		if (argc == 2 && !std::strcmp(argv[1], "--low-hard")) {
			rlimit low{std::min(rlim_t(128), original.rlim_max), std::min(rlim_t(128), original.rlim_max)};
			check(!setrlimit(RLIMIT_NOFILE, &low), "set low hard limit");
			original = low;
		}
		if (original.rlim_max < 256) return 77;
		for (int scenario = 0; scenario < 4; ++scenario) {
			const pid_t child = fork();
			check(child >= 0, "fork test child");
			if (!child) {
				rlimit limits{256, original.rlim_max};
				if (scenario == 1) limits.rlim_max = 256;
				if (scenario == 2) limits.rlim_max = std::min(rlim_t(1024), original.rlim_max);
				if (scenario == 3) limits.rlim_cur = std::min(rlim_t(8192), original.rlim_max);
				check(!setrlimit(RLIMIT_NOFILE, &limits), "set isolated limits");
				const int baseline = fd_count();
				if (!scenario) check(!workload(), "256 descriptor baseline unexpectedly passed");
				check(!raven_darwin::prepare_fd_budget(), "prepare descriptor budget");
				rlimit after{};
				check(!getrlimit(RLIMIT_NOFILE, &after), "get prepared limits");
				int kernel_max = 0;
				size_t size = sizeof(kernel_max);
				check(!sysctlbyname("kern.maxfilesperproc", &kernel_max, &size, nullptr, 0) && kernel_max > 0, "get kernel cap");
				const rlim_t expected = std::max(limits.rlim_cur,
					std::min({rlim_t(4096), limits.rlim_max, rlim_t(kernel_max)}));
				check(after.rlim_cur == expected && after.rlim_max == limits.rlim_max, "wrong soft or hard limit");
				check(!raven_darwin::prepare_fd_budget(), "idempotent budget preparation");
				std::printf("scenario=%d soft=%llu hard=%llu fd_baseline=%d\n", scenario,
					static_cast<unsigned long long>(after.rlim_cur), static_cast<unsigned long long>(after.rlim_max), baseline);
				for (int round = 0; round < 3; ++round) {
					check(workload() == (after.rlim_cur >= rlim_t(baseline + 428)), "wrong workload result");
					check(fd_count() == baseline, "descriptor lifetime leak");
				}
				std::fflush(stdout);
				const auto soft = std::to_string(after.rlim_cur), hard = std::to_string(after.rlim_max);
				execl(argv[0], argv[0], "--inherited", soft.c_str(), hard.c_str(), nullptr);
				_exit(127);
			}
			int status = 0;
			check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "isolated child failed");
			rlimit after{};
			check(!getrlimit(RLIMIT_NOFILE, &after) && after.rlim_cur == original.rlim_cur &&
				after.rlim_max == original.rlim_max, "child changed parent limits");
		}
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
