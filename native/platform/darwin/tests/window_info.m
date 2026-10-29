// Return only the window ID/bounds of a requested Raven process, for a scoped screencapture in the presentation fixture.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3) return 2;
	@autoreleasepool {
		const int pid = atoi(argv[1]);
		NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
		for (NSDictionary *window in windows) {
			if ([window[(id)kCGWindowOwnerPID] intValue] != pid || (argc == 2 && [window[(id)kCGWindowLayer] intValue] != 0))
				continue;
			if (argc == 3 && [window[(id)kCGWindowName] length]) continue;
			NSDictionary *result = @{ @"id": window[(id)kCGWindowNumber], @"bounds": window[(id)kCGWindowBounds] };
			NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
			fwrite(json.bytes, 1, json.length, stdout);
			return 0;
		}
	}
	return 1;
}
