// A poll that does not wait and finds nothing ready costs about 13 us on macOS 26, even with no descriptors at all, where a select that
// does not wait costs 0.3 us and a poll that finds something ready 0.6 us. The runtime asks that question around the calls it serves
// (handle channels, sockets, pipes, consoles), so a poll that does not wait asks select first and answers "nothing" when select finds
// nothing; whatever select calls ready, or cannot judge, goes to the real poll for its exact events.
//
// The shortcut takes only waits for input: a hang-up or an error shows in select as readable or exceptional, and a descriptor that is not
// open fails select, so neither can be answered "nothing" by mistake. Output, which most descriptors always have room for, would send
// nearly every question on to poll anyway.
#include <poll.h>
#include <sys/select.h>
#include <dlfcn.h>

#include <cerrno>

extern "C" int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	static const auto system_poll = reinterpret_cast<int (*)(struct pollfd *, nfds_t, int)>(dlsym(RTLD_NEXT, "poll"));
	if (timeout != 0)
		return system_poll(descriptors, count, timeout);
	if (count == 0)
		return 0;
	fd_set reads, errors;
	FD_ZERO(&reads);
	FD_ZERO(&errors);
	int highest = -1;
	for (nfds_t index = 0; index < count; ++index) {
		const int descriptor = descriptors[index].fd;
		if (descriptor < 0)
			continue;
		if (descriptor >= FD_SETSIZE || (descriptors[index].events & ~(POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND)))
			return system_poll(descriptors, count, timeout);
		FD_SET(descriptor, &reads);
		FD_SET(descriptor, &errors);
		highest = descriptor > highest ? descriptor : highest;
	}
	if (highest >= 0) {
		const int saved = errno;
		timeval none{0, 0};
		if (::select(highest + 1, &reads, nullptr, &errors, &none) != 0)
			return system_poll(descriptors, count, timeout);
		errno = saved;
	}
	for (nfds_t index = 0; index < count; ++index)
		descriptors[index].revents = 0;
	return 0;
}
