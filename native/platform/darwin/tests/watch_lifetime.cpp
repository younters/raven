#include "nt/handle_channel.h"
#include <algorithm>
#include <iostream>
#include <libproc.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

static int fd_count()
{
	proc_fdinfo entries[4096];
	const int bytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, entries, sizeof(entries));
	check(bytes > 0 && bytes < int(sizeof(entries)), "cannot count watch descriptors");
	return bytes / int(sizeof(proc_fdinfo));
}

static void test_watch_lifetime()
{
	const int baseline = fd_count();
	int peak = baseline;
	for (int round = 0; round < 1000; ++round) {
		{
			std::vector<pollfd> watches;
			auto lease = raven::watch_handles(watches);
			const int live = fd_count();
			check(live == baseline + int(watches.size()), "watch snapshot has unexpected descriptor owners");
			peak = std::max(peak, live);
		}
		check(fd_count() == baseline, "watch lease leaked descriptors");
	}
	std::cout << "watch rounds=1000 fd_baseline=" << baseline << " fd_peak=" << peak << " fd_final=" << fd_count() << '\n';
}

int main()
{
	try {
		int pair[2];
		check(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair), "create watch channel");
		raven::HandleChannel channel(pair[0]);
		raven::add_child_channel(&channel);
		raven::enable_process_requests();
		test_watch_lifetime();
		raven::remove_child_channel(&channel);
		close(pair[1]);
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
