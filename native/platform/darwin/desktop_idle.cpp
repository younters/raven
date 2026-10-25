#include "desktop/windows.h"

namespace raven {

// A presentation without a pollable descriptor must receive the scheduler's idle budget. Merely checking it with budget zero from a
// wait predicate cannot drive a backend that waits for input itself. Collect only; guest callbacks run when their owning thread resumes.
void NativeWindows::poll_idle(std::chrono::milliseconds budget)
{
	const auto until = std::chrono::steady_clock::now() + budget;
	const auto events = presentation.poll(budget);
	for (const auto &event : events) {
		pending_events.push_back(event);
		// Same queue categories as event_kinds in windows.cpp; a positive-budget poll must publish new input too.
		uint32_t kinds = 0x40;
		switch (event.kind) {
		case DesktopEventKind::key: case DesktopEventKind::modifiers: kinds = 1; break;
		case DesktopEventKind::pointer_motion: kinds = 2; break;
		case DesktopEventKind::pointer_button: case DesktopEventKind::pointer_wheel: kinds = 4; break;
		case DesktopEventKind::pointer_relative: kinds = 0x400; break;
		case DesktopEventKind::message: kinds = 0x108; break;
		case DesktopEventKind::keymap: kinds = 0; break;
		default: break;
		}
		queues[owner(event.window ? event.window : focus)].changed |= kinds;
	}
	refresh_screen();
	// A backend may only poll, even for a nonzero budget. Sleep the unused part on all scheduler descriptors instead of busy-polling it.
	const auto now = std::chrono::steady_clock::now();
	if (events.empty() && now < until) {
		std::vector<pollfd> descriptors;
		if (idle_descriptors)
			idle_descriptors(descriptors);
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count();
		const timespec timeout{ns / 1000000000, ns % 1000000000};
		::ppoll(descriptors.data(), descriptors.size(), &timeout, nullptr);
		if (idle_drain)
			idle_drain();
	}
}

}
