#include "input.h"
#import <AVFoundation/AVFoundation.h>
#include <objc/runtime.h>
#include <chrono>

// Replace the permission service, never the user's TCC state. An unanswered prompt
// must leave the input unavailable without blocking or issuing duplicate requests.
static unsigned requests = 0;
static AVAuthorizationStatus status = AVAuthorizationStatusDenied;
static AVAuthorizationStatus authorization(id, SEL, AVMediaType) { return status; }
static void request(id, SEL, AVMediaType, void (^)(BOOL)) { ++requests; }
int main()
{
	const auto query = class_getClassMethod(AVCaptureDevice.class, @selector(authorizationStatusForMediaType:));
	const auto ask = class_getClassMethod(AVCaptureDevice.class, @selector(requestAccessForMediaType:completionHandler:));
	if (!query || !ask) return 1;
	method_setImplementation(query, reinterpret_cast<IMP>(authorization));
	method_setImplementation(ask, reinterpret_cast<IMP>(request));
	const auto start = std::chrono::steady_clock::now();
	{
		raven_darwin::AudioInput denied({48000,1,32,4,true});
		if (denied.ready() || requests) return 2;
	}
	status = AVAuthorizationStatusNotDetermined;
	for (int i=0; i<2; ++i) {
		raven_darwin::AudioInput pending({48000,1,32,4,true});
		if (pending.ready() || requests != 1) return 3;
	}
	return std::chrono::steady_clock::now()-start < std::chrono::seconds(1) ? 0 : 4;
}
