#include "presentation.h"
#include "cpu/native_cpu.h"
#include "desktop/windows.h"
#include "nt/host_names.h"
#include "../guest/protocol.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

namespace raven {

namespace {
constexpr mach_msg_id_t surface_message = 0x52445631;
constexpr mach_msg_id_t release_message = 0x52445632;
constexpr mach_msg_id_t retire_message = 0x52445633;

struct SurfaceMessage {
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t surface;
	mach_msg_port_descriptor_t reply;
	uint64_t window, sequence;
	uint32_t slot, width, height, format;
};

struct ReleaseMessage {
	mach_msg_header_t header;
	uint64_t sequence;
	uint32_t slot;
};

struct RetireMessage {
	mach_msg_header_t header;
	uint64_t window;
};

std::string service_name(pid_t owner)
{
	return "org.raven.dxmt." + prefix_tag() + "." + std::to_string(owner);
}

void acknowledge(mach_port_t reply, uint32_t slot, uint64_t sequence)
{
	if (!reply) return;
	ReleaseMessage message{};
	message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	message.header.msgh_size = sizeof(message);
	message.header.msgh_remote_port = reply;
	message.header.msgh_id = release_message;
	message.slot = slot;
	message.sequence = sequence;
	(void)mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
}
}

struct DarwinDXMTPresentation::State {
	NativeProcess &process;
	NativeWindows &windows;
	bool polling = false;
	uint64_t presented = 0, errors = 0;
	mach_port_t service = MACH_PORT_NULL;
	struct Displayed {
		IOSurfaceRef surface = nullptr;
		CALayer *__strong layer = nil;
		uint32_t slot = 0;
		uint64_t sequence = 0;
		mach_port_t reply = MACH_PORT_NULL;
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
		if (frame.reply) {
			acknowledge(frame.reply, frame.slot, frame.sequence);
			mach_port_deallocate(mach_task_self(), frame.reply);
			frame.reply = MACH_PORT_NULL;
		} else if (auto *control = process.darwin_control()) {
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

	void start_service()
	{
		if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service) != KERN_SUCCESS) return;
		if (mach_port_insert_right(mach_task_self(), service, service, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) return;
		const std::string name = service_name(getpid());
		name_t service_name_buffer{};
		if (name.size() >= sizeof(service_name_buffer)) return;
		std::copy(name.begin(), name.end(), service_name_buffer);
		if (bootstrap_register(bootstrap_port, service_name_buffer, service) != KERN_SUCCESS)
			std::cerr << "raven DXMT: cannot register the IOSurface presentation service\n";
	}

	bool forward(const raven_surface_frame &frame, uint32_t slot, pid_t owner)
	{
		name_t name{};
		const auto text = service_name(owner);
		if (text.size() >= sizeof(name)) return false;
		std::copy(text.begin(), text.end(), name);
		mach_port_t destination = MACH_PORT_NULL;
		if (bootstrap_look_up(bootstrap_port, name, &destination) != KERN_SUCCESS) return false;
		mach_port_t port = process.copy_unix_port(frame.port);
		if (!port) { mach_port_deallocate(mach_task_self(), destination); return false; }
		SurfaceMessage message{};
		message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
		message.header.msgh_size = sizeof(message);
		message.header.msgh_remote_port = destination;
		message.header.msgh_id = surface_message;
		message.body.msgh_descriptor_count = 2;
		message.surface.name = port;
		message.surface.disposition = MACH_MSG_TYPE_COPY_SEND;
		message.surface.type = MACH_MSG_PORT_DESCRIPTOR;
		message.reply.name = service;
		message.reply.disposition = MACH_MSG_TYPE_COPY_SEND;
		message.reply.type = MACH_MSG_PORT_DESCRIPTOR;
		message.window = frame.window;
		message.sequence = frame.sequence;
		message.slot = slot;
		message.width = frame.width;
		message.height = frame.height;
		message.format = frame.pixel_format;
		const auto result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0,
			MACH_PORT_NULL, 100, MACH_PORT_NULL);
		mach_port_deallocate(mach_task_self(), port);
		mach_port_deallocate(mach_task_self(), destination);
		return result == KERN_SUCCESS;
	}

	void retire(uint64_t window, pid_t owner)
	{
		name_t name{};
		const auto text = service_name(owner);
		if (text.size() >= sizeof(name)) return;
		std::copy(text.begin(), text.end(), name);
		mach_port_t destination = MACH_PORT_NULL;
		if (bootstrap_look_up(bootstrap_port, name, &destination) != KERN_SUCCESS) return;
		RetireMessage message{};
		message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		message.header.msgh_size = sizeof(message);
		message.header.msgh_remote_port = destination;
		message.header.msgh_id = retire_message;
		message.window = window;
		(void)mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0,
			MACH_PORT_NULL, 100, MACH_PORT_NULL);
		mach_port_deallocate(mach_task_self(), destination);
	}

	bool show(IOSurfaceRef surface, const raven_surface_frame &frame, uint32_t slot, mach_port_t reply)
	{
		const auto host = windows.host_surface(frame.window);
		if (!surface || !host || host->platform != HostPlatform::metal ||
			IOSurfaceGetWidth(surface) != frame.width || IOSurfaceGetHeight(surface) != frame.height ||
			IOSurfaceGetPixelFormat(surface) != frame.pixel_format)
			return false;
		auto old = displayed.find(frame.window);
		if (!reply && old != displayed.end() && old->second.sequence >= frame.sequence) return false;
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		if (old != displayed.end()) release(old->second);
		CALayer *parent = (__bridge CALayer *)host->surface;
		CALayer *layer = [CALayer layer];
		layer.frame = parent.bounds;
		layer.contentsScale = parent.contentsScale;
		layer.magnificationFilter = kCAFilterNearest;
		layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
		layer.contentsGravity = kCAGravityResize;
		layer.opaque = YES;
		IOSurfaceIncrementUseCount(surface);
		layer.contents = (__bridge id)surface;
		[parent addSublayer:layer];
		[CATransaction commit];
		[CATransaction flush];
		displayed[frame.window] = {surface, layer, slot, frame.sequence, reply};
		capture(surface, frame);
		++presented;
		if (auto *control = process.darwin_control())
			__atomic_fetch_add(&control->surfaces_presented, 1, __ATOMIC_RELAXED);
		if (std::getenv("RAVEN_TRACE_DXMT") && (presented == 1 || presented % 60 == 0))
			std::cerr << "raven DXMT: presented " << presented << " IOSurfaces, " << frame.width << 'x' << frame.height << '\n';
		return true;
	}

	void receive()
	{
		if (!service) return;
		for (unsigned i = 0; i < 32; ++i) {
			struct { SurfaceMessage message; mach_msg_max_trailer_t trailer; } buffer{};
			const auto result = mach_msg(&buffer.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
				sizeof(buffer), service, 0, MACH_PORT_NULL);
			if (result == MACH_RCV_TIMED_OUT) break;
			if (result != KERN_SUCCESS) break;
			if (buffer.message.header.msgh_id == release_message && buffer.message.header.msgh_size >= sizeof(ReleaseMessage)) {
				const auto &ack = reinterpret_cast<const ReleaseMessage &>(buffer.message);
				if (auto *control = process.darwin_control(); control && ack.slot < RAVEN_SURFACE_SLOTS) {
					auto &frame = control->surfaces[ack.slot];
					if (frame.sequence == ack.sequence) {
						uint32_t expected = RAVEN_SURFACE_DISPLAYED;
						__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_AVAILABLE, false,
							__ATOMIC_RELEASE, __ATOMIC_RELAXED);
					}
				}
			} else if (buffer.message.header.msgh_id == retire_message && buffer.message.header.msgh_size >= sizeof(RetireMessage)) {
				const auto &retired = reinterpret_cast<const RetireMessage &>(buffer.message);
				if (auto old = displayed.find(retired.window); old != displayed.end() && old->second.reply) {
					release(old->second);
					displayed.erase(old);
					windows.release_host_surface(retired.window);
				}
			} else if (buffer.message.header.msgh_id == surface_message &&
				buffer.message.header.msgh_size >= sizeof(SurfaceMessage) && buffer.message.body.msgh_descriptor_count == 2) {
				const auto &message = buffer.message;
				IOSurfaceRef surface = IOSurfaceLookupFromMachPort(message.surface.name);
				raven_surface_frame frame{};
				frame.window = message.window;
				frame.sequence = message.sequence;
				frame.width = message.width;
				frame.height = message.height;
				frame.pixel_format = message.format;
				if (!show(surface, frame, message.slot, message.reply.name)) {
					if (surface) CFRelease(surface);
					acknowledge(message.reply.name, message.slot, message.sequence);
					mach_port_deallocate(mach_task_self(), message.reply.name);
					++errors;
				}
				mach_port_deallocate(mach_task_self(), message.surface.name);
			} else {
				mach_msg_destroy(&buffer.message.header);
			}
		}
	}

	void poll()
	{
		if (polling) return;
		auto *control = process.darwin_control();
		if (!control) return;
		polling = true;
		try {
			receive();
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
					if (const auto foreign = foreign_window_info(frame.window))
						retire(frame.window, pid_t(foreign->process));
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
				if (const auto foreign = foreign_window_info(frame.window); foreign && !windows.host_surface(frame.window)) {
					if (forward(frame, index, pid_t(foreign->process))) {
						expected = RAVEN_SURFACE_IMPORTING;
						__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_DISPLAYED, false,
							__ATOMIC_RELEASE, __ATOMIC_RELAXED);
						continue;
					}
				}
				const mach_port_t port = process.copy_unix_port(frame.port);
				IOSurfaceRef surface = port ? IOSurfaceLookupFromMachPort(port) : nullptr;
				if (port) mach_port_deallocate(mach_task_self(), port);
				if (!show(surface, frame, index, MACH_PORT_NULL)) {
					if (surface) CFRelease(surface);
					++errors;
					__atomic_fetch_add(&control->surface_errors, 1, __ATOMIC_RELAXED);
					expected = RAVEN_SURFACE_IMPORTING;
					__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_AVAILABLE, false,
						__ATOMIC_RELEASE, __ATOMIC_RELAXED);
					if (errors == 1) std::cerr << "raven DXMT: cannot import IOSurface or find the macOS window surface\n";
					continue;
				}
				expected = RAVEN_SURFACE_IMPORTING;
				__atomic_compare_exchange_n(&frame.state, &expected, RAVEN_SURFACE_DISPLAYED, false,
					__ATOMIC_RELEASE, __ATOMIC_RELAXED);
			}
		} catch (...) { polling = false; throw; }
		polling = false;
	}
};

DarwinDXMTPresentation::DarwinDXMTPresentation(NativeProcess &process, NativeWindows &windows)
	: state(std::make_unique<State>(State{process, windows}))
{
	state->start_service();
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
	if (state->service) mach_port_destroy(mach_task_self(), state->service);
}

void DarwinDXMTPresentation::poll() { state->poll(); }
}
