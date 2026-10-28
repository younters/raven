#pragma once
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

inline bool raven_macos_gui_available()
{
	@autoreleasepool {
		CFDictionaryRef session = CGSessionCopyCurrentDictionary();
		if (!session) return false;
		CFRelease(session);
		return NSScreen.screens.count && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	}
}
