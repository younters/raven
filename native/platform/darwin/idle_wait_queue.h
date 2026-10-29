#pragma once

#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <array>
#include <cerrno>
#include <utility>
#include <span>
#include <vector>

namespace raven {

// One kqueue can be watched by one persistent CFRunLoop source. Borrowed descriptors are armed only for the current idle wait;
// EV_ONESHOT prevents a still-readable descriptor from repeatedly posting AppKit events before the scheduler can consume its data.
class DarwinIdleWaitQueue {
public:
	DarwinIdleWaitQueue() : queue(kqueue())
	{
		if (queue >= 0) fcntl(queue, F_SETFD, FD_CLOEXEC);
	}
	~DarwinIdleWaitQueue() { if (queue >= 0) close(queue); }
	DarwinIdleWaitQueue(const DarwinIdleWaitQueue &) = delete;
	DarwinIdleWaitQueue &operator=(const DarwinIdleWaitQueue &) = delete;
	int descriptor() const { return queue; }

	bool arm(std::span<const pollfd> descriptors)
	{
		disarm();
		if (queue < 0) return false;
		for (const auto &descriptor : descriptors) {
			if (descriptor.fd < 0) continue;
			for (const auto [mask, filter] : {std::pair<short, short>{POLLIN, EVFILT_READ}, {POLLOUT, EVFILT_WRITE}}) {
				if (!(descriptor.events & mask)) continue;
				struct kevent change;
				EV_SET(&change, descriptor.fd, filter, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, nullptr);
				changes.push_back(change);
			}
		}
		receipts.resize(changes.size());
		const timespec zero{};
		int result;
		do result = kevent(queue, changes.data(), int(changes.size()), receipts.data(), int(receipts.size()), &zero);
		while (result < 0 && errno == EINTR);
		bool ok = result == int(changes.size());
		for (int index = 0; index < result; ++index)
			ok &= (receipts[size_t(index)].flags & EV_ERROR) && !receipts[size_t(index)].data;
		if (!ok) disarm();
		return ok;
	}

	void disarm()
	{
		if (queue < 0 || changes.empty()) return;
		for (auto &change : changes) change.flags = EV_DELETE | EV_RECEIPT;
		if (!changes.empty()) {
			receipts.resize(changes.size());
			const timespec zero{};
			// Already-delivered one-shot registrations report ENOENT. Receipts let the remaining deletes proceed in the same batch.
			int result;
			do result = kevent(queue, changes.data(), int(changes.size()), receipts.data(), int(receipts.size()), &zero);
			while (result < 0 && errno == EINTR);
			changes.clear();
		}
		consume();
	}

	bool consume()
	{
		if (queue < 0) return false;
		std::array<struct kevent, 64> events;
		const timespec zero{};
		bool ready = false;
		int count;
		do {
			do count = kevent(queue, nullptr, 0, events.data(), int(events.size()), &zero);
			while (count < 0 && errno == EINTR);
			ready |= count > 0;
		} while (count == int(events.size()));
		return ready;
	}

private:
	int queue;
	std::vector<struct kevent> changes, receipts;
};

}
