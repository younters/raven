// Apply launcher metadata once, before AppKit registers this process with the Dock.
#import <AppKit/AppKit.h>
#include <cstdlib>

void raven_macos_app_identity()
{
	const char *name = std::getenv("RAVEN_APP_NAME");
	if (name && *name)
		NSProcessInfo.processInfo.processName = [NSString stringWithUTF8String:name];
	const char *icon = std::getenv("RAVEN_APP_ICON");
	if (icon && *icon) {
		NSImage *image = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:icon]];
		if (image)
			NSApp.applicationIconImage = image;
	}
}
