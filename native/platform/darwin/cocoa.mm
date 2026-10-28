#include "dock_policy.h"
// The macOS desktop presentation (see cocoa.h). It reports input the way a Wayland compositor does, which is what the runtime reads:
// pointer positions on the window's frame, Linux input codes for buttons and keys, and an XKB keymap with its modifier state.

#include "cocoa.h"
#include "desktop/objects.h"

void raven_macos_app_identity();
bool raven_macos_claim_foreground();
void raven_macos_program_identity();

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/QuartzCore.h>

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cocoa_cursor.h"
#include "gdi_presenter.h"

namespace {

// Linux input codes of the mouse buttons.
constexpr uint32_t button_left = 0x110, button_right = 0x111, button_middle = 0x112, button_side = 0x113, button_extra = 0x114;

// The Linux (evdev) code of each macOS virtual key code; zero where the key has none.
constexpr uint16_t evdev_codes[128] = {
	30, 31, 32, 33, 35, 34, 44, 45, 46, 47, 86, 48, 16, 17, 18, 19,         // 0x00 A S D F H G Z X C V § B Q W E R
	21, 20, 2, 3, 4, 5, 7, 6, 13, 10, 8, 12, 9, 11, 27, 24,                 // 0x10 Y T 1 2 3 4 6 5 = 9 7 - 8 0 ] O
	22, 26, 23, 25, 28, 38, 36, 40, 37, 39, 43, 51, 53, 49, 50, 52,         // 0x20 U [ I P Return L J ' K ; \ , / N M .
	15, 57, 41, 14, 0, 1, 126, 125, 42, 58, 56, 29, 54, 100, 97, 0,         // 0x30 Tab Space ` Delete - Esc RCmd Cmd Shift Caps Opt Ctrl RShift ROpt RCtrl Fn
	187, 83, 0, 55, 0, 78, 0, 69, 115, 114, 113, 98, 96, 0, 74, 188,       // 0x40 F17 KP. - KP* - KP+ - Clear VolUp VolDown Mute KP/ KPEnter - KP- F18
	189, 117, 82, 79, 80, 81, 75, 76, 77, 71, 190, 72, 73, 124, 89, 95,    // 0x50 F19 KP= KP0..KP7 F20 KP8 KP9 Yen Underscore KP,
	63, 64, 65, 61, 66, 67, 85, 87, 90, 183, 186, 184, 0, 68, 0, 88,       // 0x60 F5 F6 F7 F3 F8 F9 Eisu F11 Kana F13 F16 F14 - F10 - F12
	0, 185, 110, 102, 104, 111, 62, 107, 60, 109, 59, 105, 106, 108, 103, 0 // 0x70 - F15 Help Home PgUp Del F4 End F2 PgDn F1 Left Right Down Up
};

constexpr uint32_t evdev_left_ctrl = 29, evdev_right_ctrl = 97, evdev_left_meta = 125, evdev_right_meta = 126, evdev_caps = 58;

// The XKB layout of the current macOS keyboard input source; RAVEN_XKB_LAYOUT ("de" or "us(dvorak)") overrides it.
std::pair<std::string, std::string> keyboard_layout()
{
	if (const char *wanted = std::getenv("RAVEN_XKB_LAYOUT"); wanted && *wanted) {
		std::string layout = wanted, variant;
		if (const auto open = layout.find('('); open != std::string::npos && layout.back() == ')') {
			variant = layout.substr(open + 1, layout.size() - open - 2);
			layout.resize(open);
		}
		return {layout, variant};
	}
	static const std::map<std::string, std::pair<std::string, std::string>> known{
		{"US", {"us", ""}}, {"ABC", {"us", ""}}, {"USExtended", {"us", "intl"}}, {"USInternational-PC", {"us", "intl"}},
		{"Dvorak", {"us", "dvorak"}}, {"Colemak", {"us", "colemak"}}, {"British", {"gb", ""}}, {"British-PC", {"gb", ""}},
		{"Australian", {"us", ""}}, {"Canadian", {"ca", "eng"}}, {"Canadian-CSA", {"ca", ""}}, {"German", {"de", "mac"}},
		{"Austrian", {"at", "mac"}}, {"SwissGerman", {"ch", "de_mac"}}, {"SwissFrench", {"ch", "fr_mac"}}, {"French", {"fr", "mac"}},
		{"French-PC", {"fr", ""}}, {"Belgian", {"be", ""}}, {"Spanish", {"es", "mac"}}, {"Spanish-ISO", {"es", "mac"}},
		{"Italian", {"it", "mac"}}, {"Italian-Pro", {"it", "mac"}}, {"Portuguese", {"pt", "mac"}}, {"Brazilian", {"br", ""}},
		{"Dutch", {"nl", "mac"}}, {"Swedish", {"se", "mac"}}, {"Swedish-Pro", {"se", "mac"}}, {"Norwegian", {"no", "mac"}},
		{"Danish", {"dk", "mac"}}, {"Finnish", {"fi", "mac"}}, {"Polish", {"pl", ""}}, {"PolishPro", {"pl", ""}}, {"Czech", {"cz", ""}},
		{"Hungarian", {"hu", ""}}, {"Russian", {"ru", "mac"}}, {"RussianWin", {"ru", ""}}, {"Ukrainian", {"ua", ""}},
		{"Turkish", {"tr", ""}}, {"Greek", {"gr", ""}}, {"Hebrew", {"il", ""}}, {"Icelandic", {"is", "mac"}}};
	std::string name;
	if (TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource()) {
		if (auto identifier = static_cast<CFStringRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceID))) {
			char buffer[256];
			if (CFStringGetCString(identifier, buffer, sizeof buffer, kCFStringEncodingUTF8))
				name = buffer;
		}
		CFRelease(source);
	}
	constexpr std::string_view prefix = "com.apple.keylayout.";
	if (name.starts_with(prefix))
		name.erase(0, prefix.size());
	const auto found = known.find(name);
	return found == known.end() ? std::pair<std::string, std::string>{"us", ""} : found->second;
}

}

// A window that takes the keyboard although it has no title bar.
@interface RavenWindow : NSWindow
@property (nonatomic) BOOL takesKeyboard;
@end

@implementation RavenWindow
- (BOOL)canBecomeKeyWindow { return self.takesKeyboard; }
- (BOOL)canBecomeMainWindow { return self.takesKeyboard; }
@end

// The window's frame: a layer showing the last frame presented, from its top-left corner.
@interface RavenView : NSView
@end

@implementation RavenView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
// Input is taken from the event queue before AppKit dispatches it; nothing here may beep or act on it.
- (void)keyDown:(NSEvent *)event {}
- (void)keyUp:(NSEvent *)event {}
- (void)flagsChanged:(NSEvent *)event {}
- (void)mouseDown:(NSEvent *)event {}
- (void)rightMouseDown:(NSEvent *)event {}
- (void)otherMouseDown:(NSEvent *)event {}
@end

// A graphics driver's surface over part of the client area. Pointer input passes through it to the window.
@interface RavenMetalView : NSView
@end

@implementation RavenMetalView
- (BOOL)isFlipped { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (NSView *)hitTest:(NSPoint)point { return nil; }
@end

@interface RavenApplicationDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic) raven::CocoaPresentation::State *state;
@end

// Turns what the title bar's buttons and macOS do to a window into what Windows would have told the program.
@interface RavenWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic) raven::CocoaPresentation::State *state;
@property (nonatomic) uint64_t handle;
@end

namespace raven {

struct CocoaPresentation::State {
	struct Surface {
		RavenWindow *window = nil;
		RavenView *view = nil;
		RavenMetalView *child = nil;
		bool popup = false, grabbed = false, maximized = false, close_posted = false, resized = false;
		// A window with a caption gets the title bar of macOS: the program's caption and borders, at these insets of its frames, are not shown.
		bool native = false;
		std::array<int32_t, 4> insets{};
		uint32_t style = 0;
		RavenWindowDelegate *delegate = nil;
		uint64_t parent = 0;
		int32_t popup_x = 0, popup_y = 0;
		uint32_t width = 0, height = 0, pending_width = 0, pending_height = 0;
		// The size last told to the program and when, until a frame of that size arrives: one at a time, as a compositor's configure
		// waits for its acknowledgement, so a resize never lands in the middle of the program drawing the previous one.
		uint32_t told_width = 0, told_height = 0;
		std::chrono::steady_clock::time_point told{}, last_present{};
		// Whether a frame of the size told has arrived: the next size waits for the program to stop drawing after it.
		bool told_answered = false;
		// When the size asked for last changed, and how long the program usually takes to draw a size it is told: the pacing waits
		// about that long, and sends the final size at once when the edge stops moving.
		std::chrono::steady_clock::time_point pending_changed{};
		std::chrono::duration<double, std::milli> answer{16};
		uint32_t shown_width = 0, shown_height = 0;
		NSRect restored{};
		darwin::GDIPresenter presenter;
		~Surface()
		{
			[child removeFromSuperview];
			if (NSWindow *owner = window.parentWindow)
				[owner removeChildWindow:window];
			window.delegate = nil;
			[window orderOut:nil];
			[window close];
		}
	};

	bool connected = false;
	std::chrono::steady_clock::time_point next_pump{};
	std::optional<std::pair<int32_t, int32_t>> screen_size;
	id screen_observer = nil;
	std::vector<CFFileDescriptorRef> wait_descriptors;
	RavenApplicationDelegate *delegate = nil;
	std::map<uint64_t, std::unique_ptr<Surface>> surfaces;
	std::map<NSInteger, uint64_t> numbers;
	std::set<uint64_t> maximized;
	std::set<uint64_t> alpha_windows;
	std::set<uint64_t> dock_windows;
	bool reconcile_dock = false;
	std::map<uint64_t,bool> visibility_intent;
	bool activate_on_present = false;
	bool attention_on_present = false;
	std::vector<DesktopEvent> pending;
	uint64_t pointer_window = 0, keyboard_window = 0;
	bool keyboard_known = false;
	NSEvent *last_press = nil;
	NSPoint cascade = NSZeroPoint;
	// Pointer constraint: the window it applies to, its rectangle on the frame, and whether the pointer is held still.
	uint64_t constrained = 0;
	std::array<int32_t, 4> constraint{};
	bool locked = false;
	// Command acts as Control unless RAVEN_COMMAND_KEY=windows: Windows programs' shortcuts then sit where a Mac user's fingers expect them.
	bool command_is_control = true;
	NSInteger published_change = -1;
	// Title bars drawn by macOS unless RAVEN_MACOS_FRAME=windows, and the frame the program described last for each window.
	bool native_frames = true;
	std::map<uint64_t, std::pair<std::array<int32_t, 4>, uint32_t>> frames;
	bool presenting = false;
	// A resize from the window's edges in progress: the window, which edges (left 1, right 2, bottom 4, top 8), where it began.
	uint64_t resizing = 0;
	int resize_edges = 0;
	NSPoint resize_start{};
	NSRect resize_from{};
	bool resize_cursor = false;
	CocoaCursor cursor;
	// Guest frame coordinates are device pixels for aware programs and 96-DPI pixels for unaware ones.
	double scale = 1;
	// RAVEN_TRACE_COCOA=1 reports sizes and events.
	const bool trace = std::getenv("RAVEN_TRACE_COCOA") != nullptr;
	// Diagnostics: RAVEN_COCOA_DUMP=<directory> writes each frame presented as a PPM image; RAVEN_COCOA_RESIZE_TEST=1 resizes the first
	// window from its right edge in steps a few seconds after it is shown, as a drag would.
	const char *dump_directory = std::getenv("RAVEN_COCOA_DUMP");
	unsigned dumped = 0;
	const bool resize_test = std::getenv("RAVEN_COCOA_RESIZE_TEST") != nullptr;
	// Resize tuning: RAVEN_COCOA_RESIZE_MS=<n> waits at most n ms for the program to draw a size (default: twice its usual time,
	// 10-100 ms); RAVEN_COCOA_RESIZE_BLEND=<seconds> is the crossfade between frames of different sizes (default 0.06, 0 for none).
	const double resize_patience = std::getenv("RAVEN_COCOA_RESIZE_MS") ? std::atof(std::getenv("RAVEN_COCOA_RESIZE_MS")) : 0;
	const double resize_blend = std::getenv("RAVEN_COCOA_RESIZE_BLEND") ? std::atof(std::getenv("RAVEN_COCOA_RESIZE_BLEND")) : 0.06;
	std::chrono::steady_clock::time_point first_shown{}, last_step{};
	int resize_steps = 0;
	double wheel = 0, horizontal_wheel = 0;
	bool control_click = false;
	bool host_press = false;
	NSString *double_click_action = nil;
	uint64_t unmaximizing = 0;
	NSPoint unmaximize_start{}, unmaximize_offset{};
	NSEvent *unmaximize_press = nil;
	bool unmaximize_release = false;
	bool unmaximize_moved = false;

	xkb_context *context = nullptr;
	xkb_keymap *keymap = nullptr;
	xkb_state *keys = nullptr;
	std::array<uint32_t, 4> modifiers{};
	std::set<uint32_t> held;

	~State()
	{
		watch_waits({});
		if (screen_observer)
			[NSNotificationCenter.defaultCenter removeObserver:screen_observer];
		surfaces.clear();
		if (locked)
			CGAssociateMouseAndMouseCursorPosition(true);
		if (keys)
			xkb_state_unref(keys);
		if (keymap)
			xkb_keymap_unref(keymap);
		if (context)
			xkb_context_unref(context);
	}

	void push(DesktopEvent event)
	{
		if (pending.size() >= 10000)
			throw std::runtime_error("native input queue exhaustion");
		pending.push_back(std::move(event));
	}

	void connect()
	{
		if (connected)
			return;
		if (![NSThread isMainThread])
			throw std::runtime_error("the macOS desktop runs on the main thread");
		[NSApplication sharedApplication];
		raven_macos_app_identity();
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		// The title-bar double click is the program's (maximize, as on Windows): AppKit's own action would resize the window behind its
		// back. The user's choice is kept to decide what the double click does.
		double_click_action = [[NSUserDefaults.standardUserDefaults persistentDomainForName:NSGlobalDomain][@"AppleActionOnDoubleClick"] copy];
		[NSUserDefaults.standardUserDefaults setVolatileDomain:@{@"AppleActionOnDoubleClick": @"None"} forName:NSArgumentDomain];
		delegate = [RavenApplicationDelegate new];
		delegate.state = this;
		NSApp.delegate = delegate;
		// The application menu: quitting asks the program's windows to close, as logging off Windows would.
		NSMenu *bar = [NSMenu new];
		NSMenuItem *item = [NSMenuItem new];
		[bar addItem:item];
		NSMenu *menu = [NSMenu new];
		[menu addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
		[menu addItem:[NSMenuItem separatorItem]];
		[menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
		item.submenu = menu;
		NSApp.mainMenu = bar;
		[NSApp finishLaunching];
		screen_observer = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification
			object:nil queue:nil usingBlock:^(NSNotification *) { screen_size.reset(); }];
		if (const char *value = std::getenv("RAVEN_COMMAND_KEY"))
			command_is_control = std::string_view(value) != "windows";
		if (const char *value = std::getenv("RAVEN_MACOS_FRAME"))
			native_frames = std::string_view(value) != "windows";
		connected = true;
		load_keymap();
	}

	void presence(uint64_t handle, DesktopWindowPresence facts)
	{
		if (darwin::dock_eligible(facts)) {
			const bool first = dock_windows.empty();
			dock_windows.insert(handle);
			reconcile_dock = false;
			if (first) {
				[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
				raven_macos_program_identity();
				activate_on_present = raven_macos_claim_foreground();
				attention_on_present = !activate_on_present && !NSApp.active;
			}
		} else if (dock_windows.erase(handle) && dock_windows.empty()) {
			reconcile_dock = true;
		}
	}

	void reconcile_policy()
	{
		if (reconcile_dock && dock_windows.empty())
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		reconcile_dock = false;
	}

	void foreground_request(uint64_t handle)
	{
		Surface *target = surface(handle);
		if (!target) return;
		if (NSApp.active || raven_macos_claim_foreground()) {
			[target->window makeKeyAndOrderFront:nil];
			[NSApp activateIgnoringOtherApps:YES];
			[NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateAllWindows];
		} else {
			[NSApp requestUserAttention:NSInformationalRequest];
		}
	}

	void load_keymap()
	{
		const auto [layout, variant] = keyboard_layout();
		context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (!context)
			throw std::runtime_error("cannot create the XKB context");
		const xkb_rule_names names{"evdev", "pc105", layout.c_str(), variant.c_str(), nullptr};
		keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) {
			const xkb_rule_names fallback{"evdev", "pc105", "us", "", nullptr};
			keymap = xkb_keymap_new_from_names(context, &fallback, XKB_KEYMAP_COMPILE_NO_FLAGS);
		}
		if (!keymap)
			throw std::runtime_error("cannot compile an XKB keymap (is xkeyboard-config installed?)");
		keys = xkb_state_new(keymap);
		char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
		if (!text || !keys)
			throw std::runtime_error("cannot describe the XKB keymap");
		DesktopEvent event{0, 0};
		event.kind = DesktopEventKind::keymap;
		event.keymap = text;
		std::free(text);
		push(std::move(event));
	}

	uint64_t handle_of(NSWindow *window) const
	{
		if (!window)
			return 0;
		const auto found = numbers.find(window.windowNumber);
		return found == numbers.end() ? 0 : found->second;
	}

	Surface *surface(uint64_t handle)
	{
		const auto found = surfaces.find(handle);
		return found == surfaces.end() ? nullptr : found->second.get();
	}

	// The window under a screen point, among the program's.
	uint64_t window_at(NSPoint screen) const
	{
		const NSInteger number = [NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0];
		const auto found = numbers.find(number);
		return found == numbers.end() ? 0 : found->second;
	}

	// The screen rectangle that shows the program's frame from (left, top) of its insets on.
	static NSRect content_of(NSWindow *window)
	{
		return [window contentRectForFrameRect:window.frame];
	}

	// A point of a window's frame as the program draws it, from its top-left corner.
	std::pair<int32_t, int32_t> frame_point(NSWindow *window, NSPoint screen) const
	{
		const NSRect content = content_of(window);
		int32_t left = 0, top = 0;
		for (const auto &[handle, surface] : surfaces)
			if (surface->window == window) {
				left = surface->insets[0];
				top = surface->insets[1];
			}
		return {int32_t(std::floor((screen.x - content.origin.x) * scale)) + left, int32_t(std::floor((NSMaxY(content) - screen.y) * scale)) + top};
	}

	// The screen point of a point of a window's frame.
	NSPoint screen_of(const Surface &surface, int32_t x, int32_t y) const
	{
		const NSRect content = content_of(surface.window);
		return NSMakePoint(content.origin.x + (x - surface.insets[0]) / scale, NSMaxY(content) - (y - surface.insets[1]) / scale);
	}

	static NSPoint screen_point(NSEvent *event)
	{
		return event.window ? [event.window convertPointToScreen:event.locationInWindow] : event.locationInWindow;
	}

	void update_modifiers()
	{
		const std::array<uint32_t, 4> now{xkb_state_serialize_mods(keys, XKB_STATE_MODS_DEPRESSED),
			xkb_state_serialize_mods(keys, XKB_STATE_MODS_LATCHED), xkb_state_serialize_mods(keys, XKB_STATE_MODS_LOCKED),
			xkb_state_serialize_layout(keys, XKB_STATE_LAYOUT_EFFECTIVE)};
		if (now == modifiers)
			return;
		modifiers = now;
		push({0, now[0], now[1], uint64_t(now[2]) | (uint64_t(now[3]) << 32), DesktopEventKind::modifiers});
	}

	void key(uint32_t code, bool pressed, bool repeat = false)
	{
		if (!code)
			return;
		if (!repeat) {
			if (pressed ? held.contains(code) : !held.contains(code))
				return;
			if (pressed)
				held.insert(code);
			else
				held.erase(code);
			xkb_state_update_key(keys, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		}
		push({keyboard_window, code, pressed ? 1U : 0U, 0, DesktopEventKind::key});
		update_modifiers();
	}

	uint32_t translate(unsigned short key_code) const
	{
		if (key_code >= 128)
			return 0;
		const uint32_t code = evdev_codes[key_code];
		if (command_is_control && code == evdev_left_meta)
			return evdev_left_ctrl;
		if (command_is_control && code == evdev_right_meta)
			return evdev_right_ctrl;
		return code;
	}

	// Caps Lock is a lock on macOS, which reports its state rather than the key: the keymap's lock follows it.
	void sync_caps(NSEventModifierFlags flags)
	{
		const bool on = flags & NSEventModifierFlagCapsLock;
		const xkb_mod_index_t lock = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
		const bool locked_now = lock != XKB_MOD_INVALID && xkb_state_mod_index_is_active(keys, lock, XKB_STATE_MODS_LOCKED) > 0;
		if (on != locked_now) {
			key(evdev_caps, true);
			key(evdev_caps, false);
		}
	}

	// Synthesized key events can carry modifiers without separate flagsChanged events.
	void sync_modifiers(NSEventModifierFlags flags)
	{
		const auto sync = [&](NSEventModifierFlags mask, unsigned short left, unsigned short right) {
			const uint32_t l = translate(left), r = translate(right);
			if (!(flags & mask)) {
				key(l, false);
				key(r, false);
			} else if (!held.contains(l) && !held.contains(r)) {
				key(l, true);
			}
		};
		sync(NSEventModifierFlagShift, kVK_Shift, kVK_RightShift);
		sync(NSEventModifierFlagOption, kVK_Option, kVK_RightOption);
		if (command_is_control) {
			sync(NSEventModifierFlagControl | NSEventModifierFlagCommand, kVK_Control, kVK_RightControl);
		} else {
			sync(NSEventModifierFlagControl, kVK_Control, kVK_RightControl);
			sync(NSEventModifierFlagCommand, kVK_Command, kVK_RightCommand);
		}
		sync_caps(flags);
	}

	void focus_changed()
	{
		const uint64_t now = handle_of(NSApp.keyWindow);
		if (now == keyboard_window && keyboard_known)
			return;
		// Keys held when the keyboard went elsewhere are let go there.
		if (keyboard_window && !now)
			for (const auto code : std::vector<uint32_t>(held.begin(), held.end()))
				key(code, false);
		keyboard_window = now;
		keyboard_known = true;
		DesktopEvent event{now, 0, 0, 0, DesktopEventKind::keyboard_focus};
		event.held_keys.assign(held.begin(), held.end());
		push(std::move(event));
		if (now)
			sync_caps(NSEvent.modifierFlags);
	}

	void pointer(DesktopEventKind kind, uint64_t window, int32_t x, int32_t y, uint32_t message = 0, uint64_t wparam = 0)
	{
		if (!window)
			return;
		push({window, message, wparam, 0, kind, x, y});
	}

	// Holds the pointer to the constraint the program asked for.
	void constrain(NSPoint screen)
	{
		if (!constrained || locked)
			return;
		Surface *target = surface(constrained);
		if (!target)
			return;
		const auto [x, y] = frame_point(target->window, screen);
		const int32_t cx = std::clamp(x, constraint[0], std::max(constraint[0], constraint[2] - 1));
		const int32_t cy = std::clamp(y, constraint[1], std::max(constraint[1], constraint[3] - 1));
		if (cx == x && cy == y)
			return;
		const NSPoint point = screen_of(*target, cx, cy);
		const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
		CGWarpMouseCursorPosition(CGPointMake(point.x + 0.5 / scale, top - point.y + 0.5 / scale));
	}

	// Returns whether AppKit should still see the event.
	bool input(NSEvent *event)
	{
		switch (event.type) {
		case NSEventTypeKeyDown:
		case NSEventTypeKeyUp: {
			const bool down = event.type == NSEventTypeKeyDown;
			// The application's own shortcuts: Command-Q and Command-H.
			if (down && (event.modifierFlags & NSEventModifierFlagCommand) && !(event.modifierFlags & NSEventModifierFlagControl)) {
				NSString *character = event.charactersIgnoringModifiers.lowercaseString;
				if ([character isEqualToString:@"q"] || [character isEqualToString:@"h"])
					return true;
			}
			if (!keyboard_window)
				focus_changed();
			sync_modifiers(event.modifierFlags);
			key(translate(event.keyCode), down, down && event.isARepeat);
			return false;
		}
		case NSEventTypeFlagsChanged: {
			const unsigned short code = event.keyCode;
			if (code == kVK_CapsLock) {
				sync_caps(event.modifierFlags);
				return false;
			}
			NSEventModifierFlags mask = 0;
			switch (code) {
			case kVK_Shift: case kVK_RightShift: mask = NSEventModifierFlagShift; break;
			case kVK_Control: case kVK_RightControl: mask = NSEventModifierFlagControl; break;
			case kVK_Option: case kVK_RightOption: mask = NSEventModifierFlagOption; break;
			case kVK_Command: case kVK_RightCommand: mask = NSEventModifierFlagCommand; break;
			default: return false;
			}
			const uint32_t evdev = translate(code);
			// The flags tell whether any key of the kind is down; which one changed is the key code.
			const bool down = (event.modifierFlags & mask) && !held.contains(evdev);
			key(evdev, down);
			return false;
		}
		case NSEventTypeLeftMouseDown:
		case NSEventTypeRightMouseDown:
		case NSEventTypeOtherMouseDown:
		case NSEventTypeLeftMouseUp:
		case NSEventTypeRightMouseUp:
		case NSEventTypeOtherMouseUp:
		case NSEventTypeMouseMoved:
		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
		case NSEventTypeScrollWheel:
			return mouse(event);
		default:
			return true;
		}
	}

	// Which edges of a resizable window's frame a screen point is close enough to for resizing.
	int edges_at(const Surface &target, NSPoint screen) const
	{
		if (!target.native || !(target.style & 0x40000U) || target.maximized || target.popup)
			return 0;
		constexpr CGFloat zone = 5, outside = 6;
		const NSRect frame = target.window.frame;
		if (!NSPointInRect(screen, NSInsetRect(frame, -outside, -outside)))
			return 0;
		int edges = 0;
		if (screen.x < NSMinX(frame) + zone)
			edges |= 1;
		else if (screen.x > NSMaxX(frame) - zone)
			edges |= 2;
		if (screen.y < NSMinY(frame) + zone)
			edges |= 4;
		else if (screen.y > NSMaxY(frame) - zone)
			edges |= 8;
		return edges;
	}

	static NSCursor *resize_cursor_for(int edges)
	{
		NSCursorFrameResizePosition position = NSCursorFrameResizePositionBottom;
		if (edges == 1) position = NSCursorFrameResizePositionLeft;
		else if (edges == 2) position = NSCursorFrameResizePositionRight;
		else if (edges == 5) position = NSCursorFrameResizePositionBottomLeft;
		else if (edges == 6) position = NSCursorFrameResizePositionBottomRight;
		else if (edges == 8) position = NSCursorFrameResizePositionTop;
		else if (edges == 9) position = NSCursorFrameResizePositionTopLeft;
		else if (edges == 10) position = NSCursorFrameResizePositionTopRight;
		return [NSCursor frameResizeCursorFromPosition:position inDirections:NSCursorFrameResizeDirectionsAll];
	}

	void update_cursor()
	{
		// Without focus, the pointer cannot use one of our client or resize cursors.
		// Avoid a WindowServer window-at-point round trip on every idle poll.
		if (!NSApp.active) {
			cursor.apply(false, nil);
			return;
		}
		const NSPoint point = NSEvent.mouseLocation;
		Surface *target = surface(window_at(point));
		// Menus and other popups show the arrow, as on Windows, not the cursor the program last set over its own window.
		const bool client = NSApp.active && target && !target->popup && NSPointInRect(point, content_of(target->window));
		const int edges = NSApp.active && target ? edges_at(*target, point) : 0;
		cursor.apply(client || edges, edges ? resize_cursor_for(edges) : nil);
	}

	// Follows a drag of the edges: each movement resizes the window, and the program, told at once, draws the new size before the next.
	void follow_resize(NSPoint screen)
	{
		Surface *target = surface(resizing);
		if (!target)
			return;
		const CGFloat dx = screen.x - resize_start.x, dy = screen.y - resize_start.y;
		NSRect frame = resize_from;
		if (resize_edges & 2)
			frame.size.width = std::max<CGFloat>(120, resize_from.size.width + dx);
		if (resize_edges & 1) {
			frame.size.width = std::max<CGFloat>(120, resize_from.size.width - dx);
			frame.origin.x = NSMaxX(resize_from) - frame.size.width;
		}
		if (resize_edges & 8)
			frame.size.height = std::max<CGFloat>(80, resize_from.size.height + dy);
		if (resize_edges & 4) {
			frame.size.height = std::max<CGFloat>(80, resize_from.size.height - dy);
			frame.origin.y = NSMaxY(resize_from) - frame.size.height;
		}
		[target->window setFrame:NSIntegralRect(frame) display:YES];
	}

	// Dragging a maximized window's title bar restores it under the pointer and carries it along, as Windows does: past a few points
	// of movement the window takes its restored size, placed so the pointer keeps its place across the title bar.
	void follow_unmaximize(NSPoint screen)
	{
		Surface *target = surface(unmaximizing);
		if (!target)
			return;
		if (!unmaximize_moved) {
			if (std::hypot(screen.x - unmaximize_start.x, screen.y - unmaximize_start.y) < 4)
				return;
			unmaximize_moved = true;
			const NSRect from = target->window.frame;
			NSRect restored = target->restored;
			// Without a remembered frame (maximized before it had one), restore to two thirds of the maximized size.
			if (restored.size.width < 1 || restored.size.height < 1)
				restored.size = NSMakeSize(std::round(from.size.width * 2 / 3), std::round(from.size.height * 2 / 3));
			const CGFloat across = from.size.width > 0 ? (unmaximize_start.x - from.origin.x) / from.size.width : 0.5;
			restored.origin.x = screen.x - across * restored.size.width;
			restored.origin.y = screen.y + (NSMaxY(from) - unmaximize_start.y) - restored.size.height;
			restored = NSIntegralRect(restored);
			target->restored = restored;
			unmaximize_offset = NSMakePoint(screen.x - restored.origin.x, screen.y - restored.origin.y);
			if (trace)
				std::fprintf(stderr, "cocoa: unmaximize pointer %g,%g from %s to %s\n", screen.x, screen.y,
					NSStringFromRect(from).UTF8String, NSStringFromRect(restored).UTF8String);
			presenting = true;
			[target->window setFrame:restored display:NO];
			presenting = false;
			// WM_SYSCOMMAND SC_RESTORE: the program restores itself, and that restore finds the frame already where the pointer is.
			push({unmaximizing, 0x112, 0xf120});
			// The rest of the drag is the window server's, as smooth as any other title-bar drag; its release belongs to macOS too.
			if (unmaximize_press) {
				const uint64_t handle = unmaximizing;
				[target->window performWindowDragWithEvent:unmaximize_press];
				unmaximize_press = nil;
				unmaximizing = 0;
				host_press = true;
				// The program saw this press go down on its caption; the window server keeps the release, so it is given here.
				if (unmaximize_release) {
					unmaximize_release = false;
					const auto [x, y] = frame_point(target->window, NSEvent.mouseLocation);
					pointer(DesktopEventKind::pointer_button, handle, x, y, button_left, 0);
				}
			}
			return;
		}
		const NSPoint origin = NSMakePoint(std::round(screen.x - unmaximize_offset.x), std::round(screen.y - unmaximize_offset.y));
		[target->window setFrameOrigin:origin];
		target->restored.origin = origin;
	}

	bool mouse(NSEvent *event)
	{
		const NSPoint screen = screen_point(event);
		if (unmaximizing) {
			if (event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeLeftMouseUp) {
				follow_unmaximize(NSEvent.mouseLocation);
				if (event.type == NSEventTypeLeftMouseUp) {
					if (unmaximize_release && unmaximizing) {
						const auto [x, y] = frame_point(event.window ?: surface(unmaximizing)->window, NSEvent.mouseLocation);
						pointer(DesktopEventKind::pointer_button, unmaximizing, x, y, button_left, 0);
					}
					unmaximizing = 0;
					unmaximize_release = false;
				}
				return false;
			}
			if (event.type != NSEventTypeMouseMoved)
				unmaximizing = 0;
		}
		if (resizing) {
			if (event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeLeftMouseUp) {
				follow_resize(NSEvent.mouseLocation);
				if (event.type == NSEventTypeLeftMouseUp) {
					if (const auto *target = surface(resizing)) {
						const auto [x, y] = frame_point(target->window, screen);
						pointer(DesktopEventKind::pointer_button, resizing, x, y, button_left, 0);
					}
					resizing = 0;
				}
				return false;
			}
			if (event.type != NSEventTypeMouseMoved)
				resizing = 0;
		}
		// A double click on the title bar maximizes or restores through the program, as Windows does, unless the Mac is set to
		// minimize or do nothing on a title-bar double click instead.
		if (Surface *under = surface(handle_of(event.window)); under && event.type == NSEventTypeLeftMouseDown && event.clickCount == 2 &&
			under->native && !under->popup) {
			const NSPoint point = NSEvent.mouseLocation;
			NSView *frame_view = under->window.contentView.superview;
			NSView *hit = [frame_view hitTest:[frame_view convertPoint:event.locationInWindow fromView:nil]];
			if (point.y >= NSMaxY(content_of(under->window)) && NSPointInRect(point, under->window.frame) && ![hit isKindOfClass:NSControl.class]) {
				unmaximizing = 0;
				if (trace)
					std::fprintf(stderr, "cocoa: title double click, window %s\n", NSStringFromRect(under->window.frame).UTF8String);
				NSString *action = double_click_action;
				if ([action isEqualToString:@"Minimize"])
					push({handle_of(event.window), 0x112, 0xf020});
				else if (![action isEqualToString:@"None"])
					push({handle_of(event.window), 0x112, uint64_t(under->maximized ? 0xf120 : 0xf030)});
				return false;
			}
		}
		if (Surface *under = surface(handle_of(event.window)); under && event.type == NSEventTypeLeftMouseDown && event.clickCount == 1 &&
			under->native && under->maximized && !under->popup) {
			const NSPoint point = NSEvent.mouseLocation;
			const NSRect content = content_of(under->window);
			NSView *frame_view = under->window.contentView.superview;
			NSView *hit = [frame_view hitTest:[frame_view convertPoint:event.locationInWindow fromView:nil]];
			// The title bar, not its buttons: those, and a double click (zoom), stay with macOS.
			if (point.y >= NSMaxY(content) && NSPointInRect(point, under->window.frame) && ![hit isKindOfClass:NSControl.class]) {
				unmaximizing = handle_of(event.window);
				unmaximize_start = point;
				unmaximize_moved = false;
				unmaximize_press = event;
				[under->window makeKeyAndOrderFront:nil];
				return false;
			}
		}
		// A press in the margin just outside a window's edges may come addressed to no window, or to the one behind.
		uint64_t edge_window = handle_of(event.window);
		if (event.type == NSEventTypeLeftMouseDown && !(surface(edge_window) && edges_at(*surface(edge_window), NSEvent.mouseLocation)))
			for (auto &[handle, window] : surfaces)
				if (window->window.visible && edges_at(*window, NSEvent.mouseLocation)) {
					edge_window = handle;
					break;
				}
		if (Surface *under = surface(edge_window)) {
			const int edges = edges_at(*under, NSEvent.mouseLocation);
			if (event.type == NSEventTypeLeftMouseDown && edges) {
				resizing = edge_window;
				resize_edges = edges;
				resize_start = NSEvent.mouseLocation;
				resize_from = under->window.frame;
				return false;
			}
			if (event.type == NSEventTypeMouseMoved) {
				if (edges) {
					[resize_cursor_for(edges) set];
					resize_cursor = true;
					return false;
				}
				if (resize_cursor) {
					[NSCursor.arrowCursor set];
					resize_cursor = false;
				}
			}
		}
		const bool press = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown ||
			event.type == NSEventTypeOtherMouseDown;
		// A press outside a popup that took the pointer closes it, and goes no further, as a compositor's popup grab does.
		if (press) {
			for (auto &[handle, window] : surfaces)
				if (window->popup && window->grabbed && !window->close_posted && handle_of(event.window) != handle) {
					const uint64_t inside = handle_of(event.window);
					bool child = false;
					for (auto &[other, popup] : surfaces)
						if (other == inside && popup->popup && popup->parent == handle)
							child = true;
					if (child)
						continue;
					push({handle, 0, 0, 0, DesktopEventKind::popup_done});
					window->close_posted = true;
					[window->window orderOut:nil];
					return false;
				}
		}
		// While a button is held the window it went down on keeps the pointer; otherwise the window under it has it.
		const bool dragging = event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeRightMouseDragged ||
			event.type == NSEventTypeOtherMouseDragged || event.type == NSEventTypeLeftMouseUp || event.type == NSEventTypeRightMouseUp ||
			event.type == NSEventTypeOtherMouseUp;
		// macOS sends pointer motion to the key window, which is the newest popup while a menu is open: the window under the pointer
		// is the one it is over, so moving back onto a parent menu or the menu bar reaches them.
		uint64_t window = dragging && pointer_window ? pointer_window : 0;
		if (!window && event.type == NSEventTypeMouseMoved)
			window = window_at(screen);
		if (!window)
			window = handle_of(event.window);
		if (!window)
			window = window_at(screen);
		if (!dragging)
			pointer_window = window;
		Surface *target = surface(window);
		if (!target)
			return true;
		// A press on the title bar or outside the content belongs to macOS, and so do its drag and release: the program never saw it
		// go down, and moving the window must not look to it like a button held over its client area.
		if (target->native && !dragging && !NSPointInRect(screen, content_of(target->window))) {
			if (press)
				host_press = true;
			return true;
		}
		if (host_press && dragging) {
			if (event.type == NSEventTypeLeftMouseUp || event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeOtherMouseUp)
				host_press = false;
			return true;
		}
		constrain(screen);
		const auto [x, y] = frame_point(target->window, screen);
		if (event.type != NSEventTypeScrollWheel && (event.deltaX || event.deltaY) && (event.type == NSEventTypeMouseMoved || dragging))
			push({window, 0, 0, 0, DesktopEventKind::pointer_relative, int32_t(std::lround(event.deltaX * scale)), int32_t(std::lround(event.deltaY * scale))});
		pointer(DesktopEventKind::pointer_motion, window, x, y);
		switch (event.type) {
		case NSEventTypeLeftMouseDown:
			last_press = event;
			// Control-click is the Mac's right click.
			control_click = (event.modifierFlags & NSEventModifierFlagControl) && !command_is_control;
			pointer(DesktopEventKind::pointer_button, window, x, y, control_click ? button_right : button_left, 1);
			break;
		case NSEventTypeLeftMouseUp:
			// The release of a Control-click is the right button's, whether Control is still held or not.
			pointer(DesktopEventKind::pointer_button, window, x, y, control_click ? button_right : button_left, 0);
			control_click = false;
			break;
		case NSEventTypeRightMouseDown:
		case NSEventTypeRightMouseUp:
			pointer(DesktopEventKind::pointer_button, window, x, y, button_right, event.type == NSEventTypeRightMouseDown);
			break;
		case NSEventTypeOtherMouseDown:
		case NSEventTypeOtherMouseUp: {
			const NSInteger number = event.buttonNumber;
			const uint32_t button = number == 2 ? button_middle : number == 3 ? button_side : number == 4 ? button_extra : 0;
			if (button)
				pointer(DesktopEventKind::pointer_button, window, x, y, button, event.type == NSEventTypeOtherMouseDown);
			break;
		}
		case NSEventTypeScrollWheel: {
			// A wheel reports lines, a trackpad points; a notch is 120 to Windows, and about ten points of a trackpad's travel.
			const double scale = event.hasPreciseScrollingDeltas ? 12.0 : 120.0;
			wheel += event.scrollingDeltaY * scale;
			horizontal_wheel -= event.scrollingDeltaX * scale;
			const auto take = [&](double &remainder, uint64_t horizontal) {
				const int32_t delta = int32_t(remainder / 120) * 120;
				if (!delta && !(event.hasPreciseScrollingDeltas == NO && remainder != 0))
					return;
				const int32_t sent = delta ? delta : (remainder > 0 ? 120 : -120);
				remainder -= sent;
				pointer(DesktopEventKind::pointer_wheel, window, x, y, uint32_t(sent), horizontal);
			};
			take(wheel, 0);
			take(horizontal_wheel, 1);
			break;
		}
		default:
			break;
		}
		// AppKit would resize a borderless resizable window from a press near its edge, in a loop the program cannot draw during:
		// the program has the press, and its own hit test decides whether it sizes the window.
		if (event.type == NSEventTypeLeftMouseDown && !target->native && (target->style & 0x40000U) &&
			!NSPointInRect(screen, NSInsetRect(target->window.frame, 6, 6))) {
			if (!target->window.keyWindow)
				[target->window makeKeyAndOrderFront:nil];
			return false;
		}
		return true;
	}

	void watch_waits(std::span<const pollfd> descriptors)
	{
		for (const auto descriptor : wait_descriptors) {
			CFFileDescriptorInvalidate(descriptor);
			CFRelease(descriptor);
		}
		wait_descriptors.clear();
		std::map<int, short> combined;
		for (const auto &descriptor : descriptors)
			if (descriptor.fd >= 0) combined[descriptor.fd] |= descriptor.events;
		for (const auto &[descriptor, events] : combined) {
			const CFOptionFlags kinds = ((events & POLLIN) ? kCFFileDescriptorReadCallBack : 0) |
				((events & POLLOUT) ? kCFFileDescriptorWriteCallBack : 0);
			if (!kinds) continue;
			const auto wake = [](CFFileDescriptorRef, CFOptionFlags, void *) {
				// nextEvent runs the main CFRunLoop, but a descriptor callback alone does not return an NSEvent to its caller.
				[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
					timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
			};
			const auto watched = CFFileDescriptorCreate(kCFAllocatorDefault, descriptor, false, wake, nullptr);
			if (!watched) continue;
			const auto source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, watched, 0);
			if (source) {
				CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
				CFRelease(source);
				CFFileDescriptorEnableCallBacks(watched, kinds);
				wait_descriptors.push_back(watched);
			} else CFRelease(watched);
		}
	}

	void pump(std::chrono::milliseconds budget)
	{
		// PeekMessage and scheduler predicates can ask several times per API call. AppKit has no pollable descriptor: bound the
		// nonblocking checks, while an idle poll still receives its full wait budget.
		const auto now = std::chrono::steady_clock::now();
		if (budget.count() == 0 && now < next_pump)
			return;
		next_pump = now + std::chrono::milliseconds(4);
		NSDate *until = budget.count() > 0 ? [NSDate dateWithTimeIntervalSinceNow:double(budget.count()) / 1000] : [NSDate distantPast];
		for (bool first = true;; first = false) {
			@autoreleasepool {
				NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:first ? until : [NSDate distantPast]
					inMode:NSDefaultRunLoopMode dequeue:YES];
				if (!event)
					break;
				if (input(event))
					[NSApp sendEvent:event];
				focus_changed();
			}
		}
		[NSApp updateWindows];
		focus_changed();
		update_cursor();
	}

	static NSWindowStyleMask style_mask(bool native, uint32_t style)
	{
		// A program-drawn frame with sizing borders is resizable too, for window tiling and window managers; its edges are the
		// program's own (WM_NCHITTEST), and presses near them never reach AppKit's resize loop (see mouse).
		if (!native)
			return NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable | ((style & 0x40000U) ? NSWindowStyleMaskResizable : 0);
		NSWindowStyleMask mask = NSWindowStyleMaskTitled;
		if (style & 0x80000U)
			mask |= NSWindowStyleMaskClosable;
		if (style & 0x20000U)
			mask |= NSWindowStyleMaskMiniaturizable;
		// Resizable, so the zoom button, window tiling and window managers such as Magnet and Rectangle can resize it; those resizes
		// reach the program like any other. A drag of the edges never starts AppKit's own resize loop, during which the program could
		// not draw: edges_at claims the edges and the margin AppKit resizes from outside them, and they are followed here instead.
		if (style & 0x40000U)
			mask |= NSWindowStyleMaskResizable;
		return mask;
	}

	RavenWindow *make_window(uint32_t width, uint32_t height, bool keyboard, NSWindowStyleMask mask = NSWindowStyleMaskBorderless |
		NSWindowStyleMaskMiniaturizable)
	{
		RavenWindow *window = [[RavenWindow alloc] initWithContentRect:NSMakeRect(0, 0, width / scale, height / scale)
			styleMask:mask backing:NSBackingStoreBuffered defer:NO];
		window.takesKeyboard = keyboard;
		window.releasedWhenClosed = NO;
		// The zoom button maximizes, as the program's own would; it does not take the window full screen.
		window.collectionBehavior = (window.collectionBehavior & ~NSWindowCollectionBehaviorFullScreenPrimary) | NSWindowCollectionBehaviorFullScreenNone;
		window.acceptsMouseMovedEvents = YES;
		window.hasShadow = YES;
		window.opaque = YES;
		window.backgroundColor = NSColor.windowBackgroundColor;
		RavenView *view = [[RavenView alloc] initWithFrame:NSMakeRect(0, 0, width / scale, height / scale)];
		view.wantsLayer = YES;
		// A layer's own geometry is not flipped: its bottom-left is the window's top-left.
		view.layer.contentsGravity = kCAGravityBottomLeft;
		view.layer.magnificationFilter = kCAFilterNearest;
		view.layer.contentsScale = scale;
		view.layer.opaque = YES;
		view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		window.contentView = view;
		return window;
	}

	void show(Surface &target, std::span<const uint8_t> pixels, uint32_t width, uint32_t height, bool alpha)
	{
		target.window.opaque = !alpha;
		target.window.backgroundColor = alpha ? NSColor.clearColor : NSColor.windowBackgroundColor;
		target.view.layer.opaque = !alpha;
		const auto &inset = target.insets;
		const bool cropped = inset[0] || inset[1] || inset[2] || inset[3];
		// Metal presents opaque frames; a window with per-pixel alpha (Firefox's rounded popups) keeps the premultiplied CGImage.
		IOSurfaceRef surface = alpha ? nullptr : static_cast<IOSurfaceRef>(target.presenter.frame(pixels, width, height));
		if (surface && target.view.layer.contents == (__bridge id)surface && target.shown_width == width && target.shown_height == height)
			return;
		CFDataRef data = nullptr;
		CGDataProviderRef provider = nullptr;
		CGColorSpaceRef space = nullptr;
		CGImageRef image = nullptr, shown = nullptr;
		if (!surface) {
			data = CFDataCreate(nullptr, pixels.data(), CFIndex(pixels.size()));
			provider = CGDataProviderCreateWithCFData(data);
			space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
			image = CGImageCreate(width, height, 8, 32, size_t(width) * 4, space,
				CGBitmapInfo(kCGBitmapByteOrder32Little) | CGBitmapInfo(alpha ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst),
				provider, nullptr, false, kCGRenderingIntentDefault);
			shown = image;
			if (cropped && uint32_t(inset[0] + inset[2]) < width && uint32_t(inset[1] + inset[3]) < height)
				shown = CGImageCreateWithImageInRect(image, CGRectMake(inset[0], inset[1], width - inset[0] - inset[2], height - inset[1] - inset[3]));
		}
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		if (surface) {
			// Protect a submitted surface even before the render server has acquired it. CA completion
			// releases this reservation; IsInUse then covers WindowServer and animation readers.
			CFRetain(surface);
			IOSurfaceIncrementUseCount(surface);
			[CATransaction setCompletionBlock:^{
				IOSurfaceDecrementUseCount(surface);
				CFRelease(surface);
			}];
		}
		// A frame of another size during a resize blends into the one before it rather than snapping.
		if (resize_blend > 0 && target.shown_width && (target.shown_width != width || target.shown_height != height) &&
			std::chrono::steady_clock::now() - target.pending_changed < std::chrono::milliseconds(300)) {
			CATransition *blend = [CATransition animation];
			blend.type = kCATransitionFade;
			blend.duration = resize_blend;
			[target.view.layer addAnimation:blend forKey:@"raven.resize"];
		}
		target.shown_width = width;
		target.shown_height = height;
		target.view.layer.contentsScale = scale;
		target.view.layer.contentsRect = surface && cropped ? CGRectMake(double(inset[0]) / width, double(inset[1]) / height,
			double(width - inset[0] - inset[2]) / width, double(height - inset[1] - inset[3]) / height) : CGRectMake(0, 0, 1, 1);
		target.view.layer.contents = surface ? (__bridge id)surface : (__bridge id)shown;
		[CATransaction commit];
		if (alpha)
			[target.window invalidateShadow];
		if (shown != image)
			CGImageRelease(shown);
		if (image) CGImageRelease(image);
		if (space) CGColorSpaceRelease(space);
		if (provider) CGDataProviderRelease(provider);
		if (data) CFRelease(data);
	}

	// Gives the window the content size that shows a frame of this size, keeping its top-left corner where it is, as Windows does when a
	// window changes size.
	void set_size(Surface &target, uint32_t width, uint32_t height)
	{
		const auto &inset = target.insets;
		const NSSize size = NSMakeSize(std::max<int32_t>(int32_t(width) - inset[0] - inset[2], 1) / scale,
			std::max<int32_t>(int32_t(height) - inset[1] - inset[3], 1) / scale);
		NSRect content = content_of(target.window);
		if (content.size.width == size.width && content.size.height == size.height)
			return;
		const CGFloat top = NSMaxY(content);
		content.size = size;
		content.origin.y = top - size.height;
		if (trace)
			std::fprintf(stderr, "cocoa: set_size moves window to %gx%g (was %gx%g)\n", size.width, size.height,
				content_of(target.window).size.width, content_of(target.window).size.height);
		presenting = true;
		[target.window setFrame:[target.window frameRectForContentRect:content] display:NO];
		presenting = false;
	}

	// What the program's frame measures for the window's content size now.
	std::pair<uint32_t, uint32_t> frame_size(const Surface &target) const
	{
		const NSRect content = content_of(target.window);
		return {uint32_t(std::lround(content.size.width * scale)) + uint32_t(target.insets[0] + target.insets[2]),
			uint32_t(std::lround(content.size.height * scale)) + uint32_t(target.insets[1] + target.insets[3])};
	}

	// macOS changed the window's size (its edges, zoom, full screen): the program is told its new frame size.
	void resized_by_host(uint64_t handle)
	{
		Surface *target = surface(handle);
		if (!target || presenting || target->popup)
			return;
		const auto [width, height] = frame_size(*target);
		if (width == target->width && height == target->height)
			return;
		target->pending_width = width;
		target->pending_height = height;
		target->resized = true;
		target->pending_changed = std::chrono::steady_clock::now();
		if (trace)
			std::fprintf(stderr, "cocoa: host resized %llx to frame %ux%u\n", (unsigned long long)handle, width, height);
	}

	void register_window(uint64_t handle, Surface &target)
	{
		std::erase_if(numbers, [&](const auto &entry) { return entry.second == handle && entry.first != target.window.windowNumber; });
		numbers[target.window.windowNumber] = handle;
	}

	void forget(uint64_t handle)
	{
		visibility_intent.erase(handle);
		if (dock_windows.erase(handle) && dock_windows.empty()) reconcile_dock = true;
		std::erase_if(numbers, [&](const auto &entry) { return entry.second == handle; });
		std::erase_if(pending, [&](const DesktopEvent &event) { return event.window == handle; });
		if (pointer_window == handle)
			pointer_window = 0;
		if (constrained == handle)
			release_constraint();
		surfaces.erase(handle);

	}

	void present(uint64_t handle, std::string_view title, uint32_t width, uint32_t height, std::span<const uint8_t> pixels)
	{
		if (!handle || !width || !height || width > 8192 || height > 8192 || pixels.size() != size_t(width) * height * 4)
			throw std::runtime_error("invalid native macOS frame");
		connect();
		@autoreleasepool {
			if (Surface *existing = surface(handle); existing && existing->popup)
				forget(handle);
			const auto described = frames.find(handle);
			const auto insets = described != frames.end() ? described->second.first : std::array<int32_t, 4>{};
			const uint32_t style = described != frames.end() ? described->second.second : 0;
			// Windows without a caption (splash screens, the program's own custom frames) stay borderless.
			const bool native = native_frames && insets[1] > insets[0] && uint32_t(insets[0] + insets[2]) < width &&
				uint32_t(insets[1] + insets[3]) < height;
			Surface *target = surface(handle);
			if (target && (target->native != native || target->insets != (native ? insets : std::array<int32_t, 4>{}) ||
				target->style != style)) {
				if (trace)
					std::fprintf(stderr, "cocoa: frame change %llx native %d insets %d,%d,%d,%d style %x, window %s\n",
						(unsigned long long)handle, int(native), insets[0], insets[1], insets[2], insets[3], style,
						NSStringFromRect(target->window.frame).UTF8String);
				target->native = native;
				target->insets = native ? insets : std::array<int32_t, 4>{};
				target->style = style;
				const NSRect content = content_of(target->window);
				presenting = true;
				target->window.styleMask = style_mask(native, style);
				[target->window standardWindowButton:NSWindowZoomButton].enabled = (style & 0x10000U) != 0;
				[target->window setFrame:[target->window frameRectForContentRect:content] display:NO];
				presenting = false;
				// AppKit can replace the backing window number when its style changes.
				register_window(handle, *target);
			}
			if (!target) {
				auto created = std::make_unique<Surface>();
				created->native = native;
				created->insets = native ? insets : std::array<int32_t, 4>{};
				created->style = style;
				const auto &inset = created->insets;
				created->window = make_window(std::max<int32_t>(int32_t(width) - inset[0] - inset[2], 1),
					std::max<int32_t>(int32_t(height) - inset[1] - inset[3], 1), true, style_mask(native, style));
				created->view = (RavenView *)created->window.contentView;
				created->delegate = [RavenWindowDelegate new];
				created->delegate.state = this;
				created->delegate.handle = handle;
				created->window.delegate = created->delegate;
				[created->window standardWindowButton:NSWindowZoomButton].enabled = (style & 0x10000U) != 0;
				created->width = width;
				created->height = height;
				if (NSEqualPoints(cascade, NSZeroPoint))
					[created->window center];
				cascade = [created->window cascadeTopLeftFromPoint:cascade];
				target = created.get();
				surfaces.emplace(handle, std::move(created));
				[target->window makeKeyAndOrderFront:nil];
				register_window(handle, *target);
				// A window shown maximized remembers the frame it was created with, to restore to.
				if (maximized.contains(handle))
					maximize(handle, true);
			}
			target->window.title = [NSString stringWithUTF8String:std::string(title).c_str()] ?: @"";
			if (target->close_posted)
				target->close_posted = false;
			if (trace)
				std::fprintf(stderr, "cocoa: present %llx %ux%u (asked %ux%u)\n", (unsigned long long)handle, width, height,
					target->pending_width, target->pending_height);
			if (target->pending_width == width && target->pending_height == height)
				target->resized = false;
			target->last_present = std::chrono::steady_clock::now();
			if (target->told_width == width && target->told_height == height && !target->told_answered) {
				target->told_answered = true;
				target->answer = target->answer * 0.75 + (target->last_present - target->told) * 0.25;
			}
			// A frame drawn before a host resize (including maximize) must not undo
			// that geometry while the guest is still drawing its requested size.
			const bool stale = target->pending_width && (width != target->pending_width || height != target->pending_height) &&
				width == target->width && height == target->height;
			target->width = width;
			target->height = height;
			if (!stale)
				set_size(*target, width, height);
			show(*target, pixels, width, height, alpha_windows.contains(handle));
			if (dump_directory && dumped < 400) {
				const auto path = std::string(dump_directory) + "/frame-" + std::to_string(dumped++) + "-" + std::to_string(width) + "x" +
					std::to_string(height) + ".ppm";
				if (FILE *file = std::fopen(path.c_str(), "wb")) {
					std::fprintf(file, "P6\n%u %u\n255\n", width, height);
					for (size_t at = 0; at < pixels.size(); at += 4) {
						const uint8_t rgb[3] = {pixels[at + 2], pixels[at + 1], pixels[at]};
						std::fwrite(rgb, 1, 3, file);
					}
					std::fclose(file);
				}
			}
			if (first_shown == std::chrono::steady_clock::time_point{})
				first_shown = std::chrono::steady_clock::now();
			// A minimized program still draws; that brings it back only once Windows no longer has it minimized (WS_MINIMIZE).
			if (target->window.miniaturized && !(target->style & 0x20000000U))
				[target->window deminiaturize:nil];
			if (!target->window.visible)
				[target->window orderFront:nil];
			if (activate_on_present) {
				activate_on_present=false;
				[target->window makeKeyAndOrderFront:nil];
				[NSApp activateIgnoringOtherApps:YES];
				const bool activated=[NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateIgnoringOtherApps|NSApplicationActivateAllWindows];
				if (trace) fprintf(stderr,"foreground: activation pid=%d current=%d accepted=%d active=%d\n",getpid(),NSRunningApplication.currentApplication.processIdentifier,activated,NSApp.active);
			}
			if (attention_on_present) {
				attention_on_present = false;
				[NSApp requestUserAttention:NSInformationalRequest];
			}
		}
	}

	void popup(uint64_t handle, uint64_t parent, int32_t x, int32_t y, uint32_t width, uint32_t height, std::span<const uint8_t> pixels,
		bool grab)
	{
		if (!handle || !width || !height || width > 4096 || height > 4096 || pixels.size() != size_t(width) * height * 4)
			throw std::runtime_error("invalid native macOS popup frame");
		connect();
		@autoreleasepool {
			Surface *owner = surface(parent);
			if (!owner)
				throw std::runtime_error("macOS popup has no presented parent window");
			if (Surface *existing = surface(handle); existing && (!existing->popup || existing->parent != parent || existing->close_posted))
				forget(handle);
			Surface *target = surface(handle);
			if (!target) {
				auto created = std::make_unique<Surface>();
				created->window = make_window(width, height, grab);
				created->view = (RavenView *)created->window.contentView;
				created->popup = true;
				created->grabbed = grab;
				created->parent = parent;
				created->window.level = NSPopUpMenuWindowLevel;
				target = created.get();
				surfaces.emplace(handle, std::move(created));
			}
			target->popup_x = x;
			target->popup_y = y;
			target->width = width;
			target->height = height;
			const NSPoint corner = screen_of(*owner, x, y);
			[target->window setFrame:NSMakeRect(corner.x, corner.y - height / scale, width / scale, height / scale) display:NO];
			show(*target, pixels, width, height, alpha_windows.contains(handle));
			if (!target->window.parentWindow)
				[owner->window addChildWindow:target->window ordered:NSWindowAbove];
			[target->window orderFront:nil];
			if (grab)
				[target->window makeKeyWindow];
			register_window(handle, *target);
		}
	}

	// Follows the pointer for a move or a resize the program handed over, then gives the button back as released.
	void interactive(uint64_t handle, uint32_t action)
	{
		Surface *target = surface(handle);
		if (!target || target->popup)
			return;
		NSWindow *window = target->window;
		if (action == 0 && target->maximized) {
			// A drag of a maximized program-drawn caption restores it under the pointer, as with the title bar of macOS.
			unmaximizing = handle;
			unmaximize_start = NSEvent.mouseLocation;
			unmaximize_moved = false;
			unmaximize_press = last_press && last_press.window == window ? last_press : nil;
			unmaximize_release = true;
			return;
		}
		if (action == 0) {
			if (last_press && last_press.window == window)
				[window performWindowDragWithEvent:last_press];
		} else {
			// Follow guest-initiated resizing through the normal event pump too: a nested
			// AppKit tracking loop prevents guest frames from being presented until release.
			resizing = handle;
			resize_edges = (action == 4 || action == 5 || action == 6 ? 1 : 0) |
				(action == 8 || action == 9 || action == 10 ? 2 : 0) |
				(action == 2 || action == 6 || action == 10 ? 4 : 0) |
				(action == 1 || action == 5 || action == 9 ? 8 : 0);
			resize_start = NSEvent.mouseLocation;
			resize_from = window.frame;
			return;
		}
		const auto [x, y] = frame_point(window, NSEvent.mouseLocation);
		pointer(DesktopEventKind::pointer_motion, handle, x, y);
		pointer(DesktopEventKind::pointer_button, handle, x, y, button_left, 0);
	}

	void maximize(uint64_t handle, bool wanted)
	{
		if (wanted)
			maximized.insert(handle);
		else
			maximized.erase(handle);
		Surface *target = surface(handle);
		if (!target || target->popup || target->maximized == wanted)
			return;
		target->maximized = wanted;
		NSRect frame;
		if (wanted) {
			target->restored = target->window.frame;
			frame = (target->window.screen ?: NSScreen.mainScreen).visibleFrame;
		} else {
			frame = target->restored;
		}
		if (trace)
			std::fprintf(stderr, "cocoa: maximize %llx %d to %s\n", (unsigned long long)handle, int(wanted), NSStringFromRect(frame).UTF8String);
		presenting = true;
		[target->window setFrame:frame display:NO];
		presenting = false;
		const auto [width, height] = frame_size(*target);
		target->pending_width = width;
		target->pending_height = height;
		target->resized = true;
		target->pending_changed = std::chrono::steady_clock::now();
	}

	std::optional<HostSurface> host_surface(uint64_t handle, int32_t x, int32_t y, uint32_t width, uint32_t height)
	{
		Surface *target = surface(handle);
		if (!target || target->popup)
			return std::nullopt;
		if (!target->child) {
			target->child = [[RavenMetalView alloc] initWithFrame:NSZeroRect];
			target->child.wantsLayer = YES;
			[target->view addSubview:target->child];
		}
		target->child.frame = NSMakeRect((x - target->insets[0]) / scale, (y - target->insets[1]) / scale,
			std::max<uint32_t>(width, 1) / scale, std::max<uint32_t>(height, 1) / scale);
		CAMetalLayer *layer = (CAMetalLayer *)target->child.layer;
		layer.contentsScale = scale;
		layer.drawableSize = CGSizeMake(width, height);
		layer.magnificationFilter = kCAFilterNearest;
		HostSurface result{};
		result.surface = (__bridge void *)layer;
		result.platform = HostPlatform::metal;
		return result;
	}

	void release_constraint()
	{
		if (locked) {
			CGAssociateMouseAndMouseCursorPosition(true);
			[NSCursor unhide];
		}
		locked = false;
		constrained = 0;
	}

	void confine(uint64_t handle, std::optional<std::array<int32_t, 4>> rectangle)
	{
		release_constraint();
		if (!rectangle || !surface(handle))
			return;
		constrained = handle;
		constraint = *rectangle;
		if ((*rectangle)[2] - (*rectangle)[0] <= 2 && (*rectangle)[3] - (*rectangle)[1] <= 2) {
			locked = true;
			CGAssociateMouseAndMouseCursorPosition(false);
			[NSCursor hide];
		}
	}

	void publish(std::string_view text)
	{
		connect();
		NSPasteboard *board = NSPasteboard.generalPasteboard;
		[board clearContents];
		NSString *string = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
		[board setString:string forType:NSPasteboardTypeString];
		published_change = board.changeCount;
	}

	std::optional<std::pair<uint64_t, std::string>> fetch()
	{
		NSPasteboard *board = NSPasteboard.generalPasteboard;
		// The program's own selection is not another client's.
		if (board.changeCount == published_change)
			return std::nullopt;
		NSString *string = [board stringForType:NSPasteboardTypeString];
		if (!string)
			return std::nullopt;
		return std::make_pair(uint64_t(board.changeCount), std::string(string.UTF8String ?: ""));
	}

	std::vector<DesktopEvent> poll(std::chrono::milliseconds budget)
	{
		std::vector<DesktopEvent> events;
		if (!connected) {
			if (budget.count() > 0)
				std::this_thread::sleep_for(budget);
			return events;
		}
		pump(budget);
		reconcile_policy();
		if (resize_test && first_shown != std::chrono::steady_clock::time_point{} && resize_steps < 16) {
			const auto now = std::chrono::steady_clock::now();
			if (now - first_shown > std::chrono::seconds(4) && now - last_step > std::chrono::milliseconds(120)) {
				for (auto &[handle, window] : surfaces)
					if (!window->popup) {
						NSRect frame = window->window.frame;
						frame.size.width += resize_steps < 8 ? 37 : -29;
						[window->window setFrame:frame display:YES];
						break;
					}
				++resize_steps;
				last_step = now;
			}
		}
		// The cooperative guest is stopped while its desktop polls. Once CA releases a pool slot,
		// retry the latest persistent bitmap even if the program sends no further present (caret blink).
		for (auto &[handle, window] : surfaces)
			if (const auto pixels = window->presenter.pending_pixels(); !pixels.empty()) {
				@autoreleasepool {
					show(*window, pixels, window->width, window->height, false);
				}
			}
		events.swap(pending);
		if (trace)
			for (const auto &event : events)
				if (event.kind == DesktopEventKind::resize || event.kind == DesktopEventKind::message)
					std::fprintf(stderr, "cocoa: event %d window %llx message %x x %d y %d\n", int(event.kind),
						(unsigned long long)event.window, event.message, event.x, event.y);
		const auto now = std::chrono::steady_clock::now();
		for (auto &[handle, window] : surfaces)
			if (window->resized && !window->popup) {
				// Still waiting for the program to draw the size it was told last, unless it settled on another one meanwhile, the edge
				// stopped moving (that size is the last), or the program is taking well over its usual time.
				using milliseconds = std::chrono::duration<double, std::milli>;
				const double usual = window->answer.count();
				const milliseconds quiet{std::clamp(usual / 4, 2.0, 12.0)};
				const milliseconds patience{resize_patience > 0 ? resize_patience : std::clamp(usual * 2, 10.0, 100.0)};
				const bool settled = window->told_answered && now - window->last_present > quiet;
				const bool stopped = now - window->pending_changed > std::chrono::milliseconds(16);
				if (window->told_width && !settled && !stopped && now - window->told < patience)
					continue;
				events.push_back({handle, 0, 0, 0, DesktopEventKind::resize, int32_t(window->pending_width), int32_t(window->pending_height)});
				window->resized = false;
				window->told_width = window->pending_width;
				window->told_height = window->pending_height;
				window->told = now;
				window->told_answered = false;
			}
		return events;
	}

	// The Dock icon was clicked with every window minimized.
	void restore_all()
	{
		for (auto &[handle, window] : surfaces)
			if (!window->popup && window->window.miniaturized)
				[window->window deminiaturize:nil];
	}

	// The application was asked to quit: each window is asked to close (WM_CLOSE), which the program answers as it sees fit.
	void quit_requested()
	{
		for (auto &[handle, window] : surfaces)
			if (!window->popup)
				push({handle, 0x10});
	}
};

}

@implementation RavenWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{
	if (self.state)
		self.state->push({self.handle, 0x10});
	return NO;
}
- (void)windowDidResize:(NSNotification *)notification
{
	if (self.state && self.state->trace)
		std::fprintf(stderr, "cocoa: did resize %s\n", NSStringFromRect(((NSWindow *)notification.object).frame).UTF8String);
	if (self.state)
		self.state->resized_by_host(self.handle);
}
- (void)windowDidMiniaturize:(NSNotification *)notification
{
	// WM_SYSCOMMAND SC_MINIMIZE: the program minimizes itself as Windows would have.
	if (self.state)
		self.state->push({self.handle, 0x112, 0xf020});
}
- (void)windowDidDeminiaturize:(NSNotification *)notification
{
	if (self.state)
		self.state->push({self.handle, 0x112, 0xf120});
}
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)frame
{
	// The green button maximizes and restores as the program's own caption button would.
	if (auto *target = self.state ? self.state->surface(self.handle) : nullptr)
		self.state->push({self.handle, 0x112, uint64_t(target->maximized ? 0xf120 : 0xf030)});
	return NO;
}
@end

@implementation RavenApplicationDelegate
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
	if (self.state && !visible)
		self.state->restore_all();
	return YES;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
	if (self.state)
		self.state->quit_requested();
	return NSTerminateCancel;
}
@end

namespace raven {

CocoaPresentation::CocoaPresentation() : state(std::make_unique<State>())
{
	// Constructed on the main thread before the program starts: the one place Metal's queue may safely be made (see prepare).
	darwin::GDIPresenter::prepare();
}

CocoaPresentation::~CocoaPresentation()
{
	if (state->delegate)
		state->delegate.state = nullptr;
}

void CocoaPresentation::describe(uint64_t handle,DesktopWindowPresence facts)
{
	state->presence(handle,facts);
}

DesktopPresentation CocoaPresentation::callbacks()
{
	DesktopPresentation result;
	result.foreground = [this](uint64_t handle) { state->foreground_request(handle); };
	result.visibility = [this](uint64_t handle,bool visible) { state->visibility_intent[handle] = visible; };
	result.window_state = [this](uint64_t handle,const DesktopObjects &objects) {
		const auto *window = objects.window(handle);
		if (!window) return;
		const auto owner = objects.window_root(window->owner ? window->owner : window->shown_on);
		const auto &screen = objects.nonclient_metrics();
		const auto intended = state->visibility_intent.find(handle);
		const bool visible = intended == state->visibility_intent.end() ? objects.window_visible(handle) : intended->second;
		state->visibility_intent.erase(handle);
		const auto *owning = owner && owner != handle ? objects.window(owner) : nullptr;
		if (std::getenv("RAVEN_TRACE_DOCK")) {
			const auto *definition = objects.window_class(window->class_id);
			NSString *title = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar *>(window->title.data()) length:window->title.size()];
			fprintf(stderr,"dock-window pid=%d handle=%llx class=%s title=%s style=%x ex=%x owner=%llx rect=%d,%d,%d,%d visible=%d owner_surface=%d\n",
				getpid(),(unsigned long long)handle,definition ? definition->name.c_str() : "?",title.UTF8String ?: "",
				window->style,window->ex_style,(unsigned long long)owner,window->outer.left,window->outer.top,window->outer.right,window->outer.bottom,
				visible,owning && objects.window_visible(owner) && state->surface(owner));
		}
		state->presence(handle,DesktopWindowPresence{
			.visible = visible, .top_level = !window->parent, .message_only = window->message_only,
			.owner_visible = owning && objects.window_visible(owner) && state->surface(owner),
			.style = window->style, .ex_style = window->ex_style, .owner = owner,
			.left = window->outer.left, .top = window->outer.top, .right = window->outer.right, .bottom = window->outer.bottom,
			.screen_width = screen.screen_x, .screen_height = screen.screen_y});
	};
	result.present = [this](uint64_t handle, std::string_view title, uint32_t width, uint32_t height, std::span<const uint8_t> pixels) {
		state->present(handle, title, width, height, pixels);
	};
	result.remove = [this](uint64_t handle) {
		state->maximized.erase(handle);
		state->alpha_windows.erase(handle);
		state->frames.erase(handle);
		@autoreleasepool {
			state->forget(handle);
		}
	};
	result.alpha = [this](uint64_t handle, bool enabled) {
		if (enabled)
			state->alpha_windows.insert(handle);
		else
			state->alpha_windows.erase(handle);
	};
	result.poll = [this](std::chrono::milliseconds budget) { return state->poll(budget); };
	result.popup = [this](uint64_t handle, uint64_t parent, int32_t x, int32_t y, uint32_t width, uint32_t height, std::span<const uint8_t> pixels) {
		state->popup(handle, parent, x, y, width, height, pixels, true);
	};
	result.interactive = [this](uint64_t handle, uint32_t action) { state->interactive(handle, action); };
	result.clipboard_publish = [this](std::string_view text) { state->publish(text); };
	result.clipboard_fetch = [this]() { return state->fetch(); };
	result.host_surface = [this](uint64_t handle, int32_t x, int32_t y, uint32_t width, uint32_t height) {
		return state->host_surface(handle, x, y, width, height);
	};
	result.release_host_surface = [this](uint64_t handle) {
		if (auto *target = state->surface(handle)) {
			[target->child removeFromSuperview];
			target->child = nil;
		}
	};
	result.confine = [this](uint64_t handle, std::optional<std::array<int32_t, 4>> rectangle) { state->confine(handle, rectangle); };
	result.owned_popup = [this](uint64_t handle, uint64_t owner, int32_t x, int32_t y, uint32_t width, uint32_t height,
		std::span<const uint8_t> pixels) { state->popup(handle, owner, x, y, width, height, pixels, false); };
	result.descriptor = [] { return -1; };
	result.wait_descriptors = [this](std::span<const pollfd> descriptors) { state->watch_waits(descriptors); };
	result.host_display = [this]() -> void * {
		state->connect();
		return nullptr;
	};
	result.screen_size = [this]() -> std::optional<std::pair<int32_t, int32_t>> {
		state->connect();
		if (state->screen_size)
			return state->screen_size;
		NSScreen *screen = NSScreen.mainScreen;
		if (!screen)
			return std::nullopt;
		// The area windows may use, below the menu bar and beside the Dock: maximized windows fill it.
		state->screen_size = std::pair{int32_t(std::lround(screen.visibleFrame.size.width * state->scale)),
			int32_t(std::lround(screen.visibleFrame.size.height * state->scale))};
		return state->screen_size;
	};
	result.maximize = [this](uint64_t handle, bool wanted) { state->maximize(handle, wanted); };
	result.frame = [this](uint64_t handle, std::array<int32_t, 4> insets, uint32_t style) { state->frames[handle] = {insets, style}; };
	result.minimize = [this](uint64_t handle) {
		if (auto *target = state->surface(handle); target && !target->window.miniaturized)
			[target->window miniaturize:nil];
	};
	result.cursor = [this](uint64_t handle, int count, const DesktopIcon *image) {
		if (state->trace) std::fprintf(stderr,"cocoa: cursor %llx count %d custom %d\n",(unsigned long long)handle,count,image!=nullptr);
		state->cursor.set(handle, count, image, state->scale);
		state->update_cursor();
	};
	result.backing_scale = [this] {
		state->connect();
		return double(NSScreen.mainScreen.backingScaleFactor ?: 1);
	};
	result.render_scale = [this](double scale) { state->scale = scale; state->screen_size.reset(); };
	result.platform = HostPlatform::metal;
	return result;
}

}
