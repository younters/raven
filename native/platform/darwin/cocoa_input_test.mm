#import <AppKit/AppKit.h>
#include "cocoa.h"
#include "gui_session.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

int main()
{
	if (!raven_macos_gui_available()) return 77;
	try {
		raven::CocoaPresentation host;
		auto ui = host.callbacks();
		std::vector<uint8_t> pixels(100 * 80 * 4, 255);
		ui.present(1, "Posted keyboard input", 100, 80, pixels);
		NSWindow *window = nil;
		for (NSWindow *candidate in NSApp.windows)
			if ([candidate.title isEqualToString:@"Posted keyboard input"]) window = candidate;
		if (!window) throw std::runtime_error("missing test window");
		[window resignKeyWindow];
		ui.poll(std::chrono::milliseconds(5));
		auto send = [&](NSEventType type, unsigned short code, NSEventModifierFlags flags) {
			NSEvent *event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:0
				windowNumber:window.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:code];
			[NSApp postEvent:event atStart:NO];
			return ui.poll(std::chrono::milliseconds(5));
		};
		auto check = [](bool ok, const char *why) { if (!ok) throw std::runtime_error(why); };
		auto has_key = [](const auto &events, unsigned code, bool down) {
			return std::any_of(events.begin(), events.end(), [&](const auto &event) {
				return event.kind == raven::DesktopEventKind::key && event.window == 1 && event.message == code && event.wparam == unsigned(down);
			});
		};
		const auto down = send(NSEventTypeKeyDown, 0, NSEventModifierFlagControl);
		check(has_key(down, 30, true), "posted A down missed its window");
		check(std::none_of(down.begin(), down.end(), [](const auto &event) {
			return event.kind == raven::DesktopEventKind::keyboard_focus && !event.window;
		}), "poll immediately revoked posted keyboard focus");
		const auto idle = ui.poll(std::chrono::milliseconds(5));
		check(std::none_of(idle.begin(), idle.end(), [](const auto &event) {
			return event.kind == raven::DesktopEventKind::key || (event.kind == raven::DesktopEventKind::keyboard_focus && !event.window);
		}), "idle poll released held keys or focus");
		check(has_key(send(NSEventTypeKeyUp, 0, NSEventModifierFlagControl), 30, false), "posted A up missed its window");
		ui.owned_popup(2, 1, 0, 0, 100, 80, pixels);
		NSWindow *owner = window;
		for (NSWindow *candidate in NSApp.windows)
			if (candidate.parentWindow == owner) window = candidate;
		check(window != owner, "missing owned popup");
		const auto popup = send(NSEventTypeKeyDown, 0, 0);
		check(std::none_of(popup.begin(), popup.end(), [](const auto &event) {
			return event.kind == raven::DesktopEventKind::keyboard_focus && event.window == 2;
		}), "nonactivating popup stole keyboard focus");
		ui.remove(2);
		window = owner;
		send(NSEventTypeKeyDown, 0, 0);
		ui.remove(1);
		ui.poll(std::chrono::milliseconds(5));
		const auto removed = send(NSEventTypeKeyUp, 0, 0);
		check(std::none_of(removed.begin(), removed.end(), [](const auto &event) {
			return event.window == 1 && (event.kind == raven::DesktopEventKind::key || event.kind == raven::DesktopEventKind::keyboard_focus);
		}), "removed window retained delivered focus");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
