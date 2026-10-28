#pragma once

#include "desktop/presentation.h"
#include "dock_policy.h"
#include <memory>

namespace raven {

// The macOS desktop: every top-level window of the program is a borderless NSWindow showing the frames the runtime draws, Windows'
// own frame included, with popups as child windows and graphics surfaces as Metal layers. AppKit runs on the thread that calls it,
// which is the process's main thread; its events are taken in poll.
class CocoaPresentation {
public:
	CocoaPresentation();
	~CocoaPresentation();
	CocoaPresentation(const CocoaPresentation &) = delete;
	CocoaPresentation &operator=(const CocoaPresentation &) = delete;
	DesktopPresentation callbacks();
	void describe(uint64_t,DesktopWindowPresence);
	// Public only so the application delegate can name it.
	struct State;

private:
	std::unique_ptr<State> state;
};

}
