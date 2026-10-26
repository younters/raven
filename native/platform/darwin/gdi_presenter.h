#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace raven::darwin {

// A completed opaque frame, owned by the presenter. The caller places it in CALayer.contents.
// No CPU pixel copy: Metal reads the persistent guest bitmap and renders into a bounded output pool.
class GDIPresenter {
public:
	GDIPresenter();
	~GDIPresenter();
	// Creates the process's Metal device, queue and pipeline. Call it once on the main thread's own stack, before guest threads run:
	// Metal records a backtrace when it makes a command queue, and that walk faults on a guest thread's coroutine stack.
	static void prepare();
	void *frame(std::span<const uint8_t> pixels, uint32_t width, uint32_t height);
	// A full pool coalesces updates until Cocoa next pumps CA completions; the final frame is retried.
	std::span<const uint8_t> pending_pixels() const;
private:
	struct State;
	std::unique_ptr<State> state;
};

}
