#include "presentation.h"
#include "cpu/native_cpu.h"
#include "desktop/windows.h"
#include "../guest/protocol.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <mach/mach.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

namespace raven {

struct DarwinDXMTPresentation::State {
	NativeProcess &process;
	NativeWindows &windows;
	bool polling = false;
	uint64_t presented = 0, errors = 0;
	struct Displayed {
		IOSurfaceRef surface = nullptr;
		CALayer *__strong layer = nil;
		uint32_t slot = 0;
		uint64_t sequence = 0;
	};
	std::map<uint64_t, Displayed> displayed;

	void release(Displayed &frame)
	{
		frame.layer.contents = nil;
		[frame.layer removeFromSuperlayer];
		frame.layer = nil;
		if (frame.surface) {
			IOSurfaceDecrementUseCount(frame.surface);
			CFRelease(frame.surface);
			frame.surface = nullptr;
		}
		if (auto *control = process.darwin_control()) {
			uint32_t expected = RAVEN_SURFACE_DISPLAYED;
			__atomic_compare_exchange_n(&control->surfaces[frame.slot].state, &expected, RAVEN_SURFACE_AVAILABLE, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED);
		}
	}

	void capture(IOSurfaceRef surface, const raven_surface_frame &frame)
	{
		const char *directory = std::getenv("RAVEN_DXMT_CAPTURE_DIRECTORY");
		if (!directory || !*directory) return;
		std::filesystem::create_directories(directory);
		const auto path = std::filesystem::path(directory) / (std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".rgba");
		if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess) return;
		std::vector<uint8_t> pixels(size_t(frame.width) * frame.height * 4);
		const auto *source = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
		const auto stride = IOSurfaceGetBytesPerRow(surface);
		for (size_t y = 0; y < frame.height; ++y)
			for (size_t x = 0; x < frame.width; ++x) {
				const auto *pixel = source + y * stride + x * 4;
				auto *target = pixels.data() + (y * frame.width + x) * 4;
				target[0] = pixel[frame.pixel_format == 'BGRA' ? 2 : 0];
				target[1] = pixel[1];
				target[2] = pixel[frame.pixel_format == 'BGRA' ? 0 : 2];
				target[3] = pixel[3];
			}
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char *>(pixels.data()), std::streamsize(pixels.size()));
	}

	void poll()
	{
		if (polling) return;
		auto *control = process.darwin_control();
		if (!control) return;
		polling = true;
		try {
			std::vector<uint32_t> ready;
			for (uint32_t i = 0; i < RAVEN_SURFACE_SLOTS; ++i) {
				auto &frame = control->surfaces[i];
				const auto status = __atomic_load_n(&frame.state, __ATOMIC_ACQUIRE);
				if (status == RAVEN_SURFACE_RETIRE) {
					const auto old = displayed.find(frame.window);
					if (old != displayed.end() && old->second.slot == i) {
						release(old->second);
						displayed.erase(old);
						windows.release_host_surface(frame.window);
					}
					__atomic_store_n(&frame.state, RAVEN_SURFACE_UNUSED, __ATOMIC_RELEASE);
				} else if (status == RAVEN_SURFACE_READY) ready.push_back(i);
			}
			std::sort(ready.begin(), ready.end(), [&](uint32_t a, uint32_t b) {
				return control->surfaces[a].sequence < control->surfaces[b].sequence;
			});
			for (const auto index : ready) {
				auto &frame = control->surfaces[index];
				uint32_t expected = RAVEN_SURFACE_READY;
				if (!__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_IMPORTING, false,
					__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
				const mach_port_t port = process.copy_unix_port(frame.port);
				IOSurfaceRef surface = port ? IOSurfaceLookupFromMachPort(port) : nullptr;
				if (port) mach_port_deallocate(mach_task_self(), port);
				const auto host = windows.host_surface(frame.window);
				if (!surface || !host || host->platform != HostPlatform::metal ||
					IOSurfaceGetWidth(surface) != frame.width || IOSurfaceGetHeight(surface) != frame.height) {
					if (surface) CFRelease(surface);
					++errors;
					__atomic_fetch_add(&control->surface_errors, 1, __ATOMIC_RELAXED);
					expected = RAVEN_SURFACE_IMPORTING;
					__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_AVAILABLE, false,
						__ATOMIC_RELEASE, __ATOMIC_RELAXED);
					if (errors == 1) std::cerr << "raven DXMT: cannot import IOSurface or find the macOS window surface\n";
					continue;
				}
				auto old = displayed.find(frame.window);
				if (old != displayed.end() && old->second.sequence >= frame.sequence) {
					CFRelease(surface);
					expected = RAVEN_SURFACE_IMPORTING;
					__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_AVAILABLE, false,
						__ATOMIC_RELEASE, __ATOMIC_RELAXED);
					continue;
				}
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				if (old != displayed.end()) release(old->second);
				CALayer *parent = (__bridge CALayer *)host->surface;
				CALayer *layer = [CALayer layer];
				layer.frame = parent.bounds;
				layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
				layer.contentsGravity = kCAGravityResize;
				layer.opaque = YES;
				IOSurfaceIncrementUseCount(surface);
				layer.contents = (__bridge id)surface;
				[parent addSublayer:layer];
				[CATransaction commit];
				[CATransaction flush];
				expected = RAVEN_SURFACE_IMPORTING;
				__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_DISPLAYED, false,
					__ATOMIC_RELEASE, __ATOMIC_RELAXED);
				displayed[frame.window] = {surface, layer, index, frame.sequence};
				capture(surface, frame);
				++presented;
				__atomic_fetch_add(&control->surfaces_presented, 1, __ATOMIC_RELAXED);
				if (std::getenv("RAVEN_TRACE_DXMT") && (presented == 1 || presented % 60 == 0))
					std::cerr << "raven DXMT: presented " << presented << " IOSurfaces, " << frame.width << 'x' << frame.height << '\n';
			}
		} catch (...) { polling = false; throw; }
		polling = false;
	}
};

DarwinDXMTPresentation::DarwinDXMTPresentation(NativeProcess &process, NativeWindows &windows)
	: state(std::make_unique<State>(State{process, windows}))
{
	process.set_unix_pump([this] { poll(); });
}

DarwinDXMTPresentation::~DarwinDXMTPresentation()
{
	state->process.set_unix_pump({});
	for (auto &[window, frame] : state->displayed) {
		state->release(frame);
		state->windows.release_host_surface(window);
	}
	if (std::getenv("RAVEN_TRACE_DXMT") && state->presented)
		std::cerr << "raven DXMT: " << state->presented << " frames presented, " << state->errors << " presentation errors\n";
}

void DarwinDXMTPresentation::poll() { state->poll(); }
}
