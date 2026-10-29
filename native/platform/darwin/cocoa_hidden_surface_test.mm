#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "cocoa.h"
#include "gui_session.h"
#include <cstdio>
#include <stdexcept>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	if (!raven_macos_gui_available()) return 77;
	try {
		const pid_t foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
		raven::CocoaPresentation host;
		auto ui = host.callbacks();
		__weak CAMetalLayer *released = nil;
		@autoreleasepool {
			const auto first = ui.host_surface(1, 0, 0, 1, 1);
			check(first && first->platform == raven::HostPlatform::metal, "hidden layer was not created");
			CAMetalLayer *layer = (__bridge CAMetalLayer *)first->surface;
			released = layer;
			check(CGSizeEqualToSize(layer.drawableSize, CGSizeMake(1, 1)), "hidden layer dimensions differ");
			const auto resized = ui.host_surface(1, 0, 0, 17, 23);
			check(resized && resized->surface == first->surface && CGSizeEqualToSize(layer.drawableSize, CGSizeMake(17, 23)),
				"hidden surface resize replaced its layer");
			const auto second = ui.host_surface(2, 0, 0, 1, 1);
			check(second && second->surface != first->surface, "hidden windows share a layer");
			check(NSApp.windows.count == 0 && NSApp.keyWindow == nil && NSApp.activationPolicy == NSApplicationActivationPolicyAccessory,
				"hidden surface created a native window or Dock presence");
			check(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == foreground, "hidden surface stole activation");
			ui.release_host_surface(1);
			check(ui.host_surface(2, 0, 0, 1, 1)->surface == second->surface, "release affected another hidden surface");
			ui.release_host_surface(2);
			layer = nil;
		}
		for (unsigned attempt = 0; attempt < 2; ++attempt) {
			@autoreleasepool {
				[CATransaction flush];
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
			}
		}
		check(released == nil, "released hidden surface retained its layer");
	} catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
	return 0;
}
