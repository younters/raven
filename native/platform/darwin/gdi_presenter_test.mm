#include "desktop/shared_pixels.h"
#include "gdi_presenter.h"
#include <IOSurface/IOSurface.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	try {
		setenv("RAVEN_COCOA_PRESENT", "metal", 1);
		unsetenv("RAVEN_PARALLEL");
		for (uint32_t width : {1U, 17U, 801U}) {
			constexpr uint32_t height = 7;
			const size_t size = (size_t(width) * height * 4 + 4095) & ~size_t(4095);
			raven::SharedPixels pixels(size, width, height);
			raven::darwin::GDIPresenter presenter;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const uint32_t value = ((x & 255) << 16) | (y << 8) | 0x35;
					std::memcpy(pixels.data() + (size_t(y) * width + x) * 4, &value, 4);
				}
			const auto span = std::span<const uint8_t>(pixels.data(), size_t(width) * height * 4);
			auto first = static_cast<IOSurfaceRef>(presenter.frame(span, width, height));
			check(first, "Metal failed to present an odd-width surface");
			IOSurfaceLock(first, kIOSurfaceLockReadOnly, nullptr);
			const auto *output = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(first));
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const auto *p = output + y * IOSurfaceGetBytesPerRow(first) + x * 4;
					check(p[0] == 0x35 && p[1] == y && p[2] == (x & 255) && p[3] == 255,
						"Metal changed BGR colour, orientation, row stride or opaque alpha");
				}
			IOSurfaceUnlock(first, kIOSurfaceLockReadOnly, nullptr);
			IOSurfaceIncrementUseCount(first);
			auto second = static_cast<IOSurfaceRef>(presenter.frame(span, width, height));
			check(second && second != first, "Metal overwrote a displayed frame");
			IOSurfaceIncrementUseCount(second);
			auto third = static_cast<IOSurfaceRef>(presenter.frame(span, width, height));
			check(third && third != first && third != second, "Metal did not use its third buffer");
			IOSurfaceIncrementUseCount(third);
			check(presenter.frame(span, width, height) == third && !presenter.pending_pixels().empty(),
				"Metal did not defer a final update at pool exhaustion");
			IOSurfaceDecrementUseCount(first);
			pixels.data()[0] = 0x19;
			check(presenter.frame(presenter.pending_pixels(), width, height) == first && presenter.pending_pixels().empty(),
				"Metal did not retry a deferred final update into a released frame");
			IOSurfaceLock(first, kIOSurfaceLockReadOnly, nullptr);
			check(static_cast<uint8_t *>(IOSurfaceGetBaseAddress(first))[0] == 0x19, "Metal missed an incremental GDI update");
			IOSurfaceUnlock(first, kIOSurfaceLockReadOnly, nullptr);
			IOSurfaceDecrementUseCount(second);
			IOSurfaceDecrementUseCount(third);
		}
		std::cout << "Metal GDI stride, BGRX, opacity, incremental update and bounded buffering passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
