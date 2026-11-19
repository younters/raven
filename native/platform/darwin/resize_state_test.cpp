#include "resize_state.h"
#include <stdexcept>
int main() {
	auto check=[](bool ok) { if (!ok) throw std::runtime_error("resize transaction contract"); };
	raven::darwin::ResizeState state;
	check(!state.active() && !state.take());
	state.request(100,80); state.request(200,160);
	auto first=state.take(); check(first && first->width==200 && state.active());
	check(!state.take()); // Rendering is irrelevant to delivery.
	state.request(300,240);
	check(!state.take()); // Only one guest placement may be outstanding.
	check(!state.complete(first->generation)); // Reentrant host change wins.
	auto second=state.take(); check(second && second->width==300);
	check(state.complete(second->generation) && !state.active());
	state.request(200,160); auto third=state.take();
	state.request(300,240);
	check(!state.complete(third->generation));
	auto fourth=state.take();
	check(third && fourth && fourth->generation>second->generation);
	check(!state.complete(second->generation)); // A→B→A is not an old acknowledgement.
	check(state.complete(fourth->generation));
	state.request(300,240); state.request(300,240);
	auto fifth=state.take(); check(fifth && !state.take());
	check(state.complete(fifth->generation)); // Also used for guest-adjusted/vetoed placement.
	// Lost delivery expires without sleeping, including when no replacement is queued.
	using State = raven::darwin::ResizeState;
	const auto now = State::Clock::now();
	state.request(400,300); auto lost=state.take(now);
	check(!state.take(now+State::timeout-std::chrono::milliseconds(1)));
	check(!state.take(now+State::timeout) && !state.active());
	check(!state.complete(lost->generation));
	state.request(500,400); lost=state.take(now);
	state.request(600,500);
	auto replacement=state.take(now+State::timeout);
	check(replacement && replacement->width==600);
	check(!state.complete(lost->generation) && state.active());
	check(state.complete(replacement->generation));
}
