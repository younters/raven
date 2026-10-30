#pragma once

#include <functional>
#include <poll.h>
#include <span>
#include <utility>

namespace raven {

// The spans and their descriptor leases belong to the live poll_idle frames. Restore the nearest enclosing wait for this window
// system after a nested poll, and clear the backend only when its outermost frame leaves.
class DarwinIdleWaitScope {
public:
	DarwinIdleWaitScope(const void *owner, std::function<void(std::span<const pollfd>)> watch, std::span<const pollfd> descriptors)
		: owner(owner), watch(std::move(watch)), descriptors(descriptors), previous(active)
	{
		if (this->watch) this->watch(descriptors);
		active = this;
	}
	~DarwinIdleWaitScope()
	{
		active = previous;
		if (!watch) return;
		auto *outer = previous;
		while (outer && outer->owner != owner) outer = outer->previous;
		// A destructor may run while an exception unwinds; a failed restore only costs an early wake, so it must not terminate.
		try {
			watch(outer ? outer->descriptors : std::span<const pollfd>{});
		} catch (...) {
		}
	}
	DarwinIdleWaitScope(const DarwinIdleWaitScope &) = delete;
	DarwinIdleWaitScope &operator=(const DarwinIdleWaitScope &) = delete;

private:
	const void *owner;
	std::function<void(std::span<const pollfd>)> watch;
	std::span<const pollfd> descriptors;
	DarwinIdleWaitScope *previous;
	static inline thread_local DarwinIdleWaitScope *active = nullptr;
};

}
