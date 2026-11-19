#pragma once
#include <cstdint>
#include <chrono>
#include <optional>

namespace raven::darwin {
// Geometry is acknowledged by window placement, never by a rendered bitmap.
// At most one placement is in flight; further host changes replace the pending size.
// All accesses run on the desktop thread; callbacks may reenter it.
class ResizeState {
public:
	struct Request { uint64_t generation; uint32_t width, height; };
	void request(uint32_t width, uint32_t height) {
		if (active() && latest.width == width && latest.height == height) return;
		latest = {latest.generation + 1, width, height};
	}
	using Clock = std::chrono::steady_clock;
	static constexpr auto timeout = std::chrono::seconds(5);
	std::optional<Request> take(Clock::time_point now = Clock::now()) {
		// A lost delivery must not hold later geometry forever. Retire its generation;
		// a late completion cannot acknowledge a newer request.
		if (delivered != completed && now - sent >= timeout) completed = delivered;
		if (delivered != completed || delivered == latest.generation) return std::nullopt;
		delivered = latest.generation;
		sent = now;
		return latest;
	}
	bool complete(uint64_t generation) {
		if (generation != delivered || generation <= completed) return false;
		completed = generation;
		return generation == latest.generation;
	}
	bool active() const { return completed != latest.generation; }
	Request requested() const { return latest; }
private:
	Clock::time_point sent{};
	Request latest{};
	uint64_t delivered = 0, completed = 0;
};
}
