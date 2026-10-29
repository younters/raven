#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace raven {

class SharedPixels;

// Windows of every process of a prefix are visible to the others, as one window system serves them all on Windows: a process that
// creates a window publishes its handle and client size, and another process that draws into it (Chromium's GPU process drawing the
// browser's windows, for one) gets a device context over pixels it shares with the owner, which shows them in the window.

// The window handles of this process carry a tag of their own, so no two processes of the host hand out the same one.
uint64_t window_handle_base();
// Owner side: the window exists with this client size, or no longer exists.
void publish_window(uint64_t window, uint32_t width, uint32_t height, uint32_t thread = 0, uint64_t parent = 0);
void withdraw_window(uint64_t window);
struct ForeignWindowInfo {
	uint32_t process, thread, width, height;
	uint64_t parent=0;
};
std::optional<ForeignWindowInfo> foreign_window_info(uint64_t window);
// Commands to the owning host. The locked mailbox merges reparent/position updates until its owner consumes them.
struct ForeignWindowUpdate {
	uint64_t parent=0, after=0;
	int32_t x=0,y=0,width=0,height=0;
	uint32_t flags=0,mask=0; // bit 0: parent, bit 1: WINDOWPOS
};
bool request_window_update(uint64_t window, const ForeignWindowUpdate &update);
std::optional<ForeignWindowUpdate> take_window_update(uint64_t window, bool consume = true);
// Drawing side: the client size of a window another live process of the prefix owns.
std::optional<std::pair<uint32_t, uint32_t>> foreign_window_size(uint64_t window);

// Pixels of a window shared between the process that draws them and the owner: a header page, then 32-bit rows top down.
class ForeignSurface {
public:
	// Opens (creating or growing when needed) the shared pixels of a window for drawing.
	static std::shared_ptr<ForeignSurface> open_for_drawing(uint64_t window, uint32_t width, uint32_t height);
	~ForeignSurface();
	std::shared_ptr<SharedPixels> pixels;
	uint32_t width = 0, height = 0;
	// Marks a finished frame for the owner.
	void publish_frame();

private:
	int file = -1;
	uint32_t *header = nullptr;
};

// Owner side: when another process finished a new frame for the window, hands its pixels (32-bit rows, top down) to `use` and returns
// true; `seen` remembers the last frame taken.
bool take_foreign_frame(uint64_t window, uint32_t &seen, const std::function<void(const uint8_t *, uint32_t, uint32_t)> &use);
// Changes whenever another process starts or stops drawing into a window of the prefix (its pixels file is made or removed).
uint64_t foreign_frames_stamp();
// A descriptor that becomes readable when that may have happened, for a waiter to sleep on; drain it once woken. -1 without one.
int foreign_frames_descriptor();
void drain_foreign_frames_descriptor();

}
