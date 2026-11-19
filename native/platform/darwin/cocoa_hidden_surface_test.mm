#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "cocoa.h"
#include "gui_session.h"
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
	if (!raven_macos_gui_available()) return 77;
	try {
		const pid_t foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
		raven::CocoaPresentation host;
		auto ui = host.callbacks();
		__weak CAMetalLayer *released = nil;
		@autoreleasepool {
			const auto first = ui.host_surface(1, 1, 0, 0, 1, 1, true);
			check(first && first->platform == raven::HostPlatform::metal, "hidden layer was not created");
			CAMetalLayer *layer = (__bridge CAMetalLayer *)first->surface;
			released = layer;
			check(CGSizeEqualToSize(layer.drawableSize, CGSizeMake(1, 1)), "hidden layer dimensions differ");
			const auto resized = ui.host_surface(1, 1, 0, 0, 17, 23, true);
			check(resized && resized->surface == first->surface && CGSizeEqualToSize(layer.drawableSize, CGSizeMake(17, 23)),
				"hidden surface resize replaced its layer");
			const auto child = ui.host_surface(1, 11, 2, 3, 7, 9, false);
			check(child && child->surface != first->surface, "child window shares its root layer");
			check(ui.host_surface(1, 11, 4, 5, 9, 11, true)->surface == child->surface,
				"showing a child replaced its layer");
			ui.release_host_surface(11);
			check(ui.host_surface(1, 1, 0, 0, 17, 23, true)->surface == first->surface, "child release affected root");
			const auto second = ui.host_surface(2, 2, 0, 0, 1, 1, true);
			check(second && second->surface != first->surface, "hidden windows share a layer");
			check(NSApp.windows.count == 0 && NSApp.keyWindow == nil && NSApp.activationPolicy == NSApplicationActivationPolicyAccessory,
				"hidden surface created a native window or Dock presence");
			check(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == foreground, "hidden surface stole activation");
			ui.release_host_surface(1);
			check(ui.host_surface(2, 2, 0, 0, 1, 1, true)->surface == second->surface, "release affected another hidden surface");
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
		if (argc > 1 && std::string_view(argv[1]) == "--show-transition") {
			std::vector<uint8_t> pixels(40 * 30 * 4, 255);
			const auto regular = ui.host_surface(3, 3, 0, 0, 40, 30, true);
			ui.present(3, "Hidden surface transition", 40, 30, pixels);
			check(ui.host_surface(3, 3, 0, 0, 40, 30, true)->surface == regular->surface,
				"showing a normal window replaced its Vulkan layer");
			const auto popup = ui.host_surface(4, 4, 0, 0, 40, 30, true);
			ui.owned_popup(4, 3, 0, 0, 40, 30, pixels);
			check(ui.host_surface(4, 4, 0, 0, 40, 30, true)->surface == popup->surface,
				"showing an owned popup replaced its Vulkan layer");
			// A popup can acquire a regular host before its owner is presented.
			// Conversion in either direction must retain the swap chain's layer
			// and its already submitted contents, without another GPU Present.
			ui.present(4, "Popup before owner", 40, 30, pixels);
			check(ui.host_surface(4, 4, 0, 0, 40, 30, true)->surface == popup->surface,
				"converting a popup to a regular host replaced its layer");
			CAMetalLayer *popup_layer = (__bridge CAMetalLayer *)popup->surface;
			CALayer *submitted = [CALayer layer];
			submitted.backgroundColor = NSColor.greenColor.CGColor;
			submitted.frame = CGRectMake(0, 0, 40, 30);
			[popup_layer addSublayer:submitted];
			ui.owned_popup(4, 3, 0, 0, 40, 30, pixels);
			check(ui.host_surface(4, 4, 0, 0, 40, 30, true)->surface == popup->surface &&
				submitted.superlayer == popup_layer,
				"converting a regular host to an owned popup lost the submitted image");
			NSView *popup_view = nil;
			for (NSWindow *window in NSApp.windows)
				for (NSView *view in window.contentView.subviews)
					if (view.layer == popup_layer) popup_view = view;
			check(popup_view && popup_view.superview, "converted popup layer was not reattached to its host");
			const auto hosted_child = ui.host_surface(3, 31, 1, 2, 10, 12, true);
            CAMetalLayer *child_layer = (__bridge CAMetalLayer *)hosted_child->surface;
            NSView *child_view = nil;
            for (NSWindow *window in NSApp.windows)
                for (NSView *view in window.contentView.subviews)
                    if (view.layer == child_layer) child_view = view;
            check(child_view && child_view.superview, "hosted child was not attached");
            ui.remove(31);
            check(child_view.superview == nil, "destroyed child retained its layer in parent");
            ui.remove(4);
			ui.remove(3);
		}
	} catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
	return 0;
}
