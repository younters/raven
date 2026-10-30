#include "input.h"
#import <AVFoundation/AVFoundation.h>
#include <objc/runtime.h>
#include <chrono>
#include <AudioToolbox/AudioToolbox.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <string_view>

// Replace the permission service, never the user's TCC state. An unanswered prompt
// must leave the input unavailable without blocking or issuing duplicate requests.
static unsigned requests = 0;
static AVAuthorizationStatus status = AVAuthorizationStatusDenied;
static AVAuthorizationStatus authorization(id, SEL, AVMediaType) { return status; }
static void (^completion)(BOOL);
static void request(id, SEL, AVMediaType, void (^reply)(BOOL)) { ++requests; completion = [reply copy]; }
// Mock the capture boundary too: an authorized open must reach it without
// changing TCC or opening the machine's microphone.
static unsigned opens=0, buffers=0, disposals=0;
static AudioQueueBuffer buffer{};
extern "C" OSStatus AudioQueueNewInput(const AudioStreamBasicDescription *, AudioQueueInputCallback, void *,
	CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *queue)
{
	++opens;
	*queue=reinterpret_cast<AudioQueueRef>(&buffer);
	return 0;
}
extern "C" OSStatus AudioQueueAllocateBuffer(AudioQueueRef, UInt32, AudioQueueBufferRef *out)
{
	++buffers; *out=&buffer; return 0;
}
extern "C" OSStatus AudioQueueEnqueueBuffer(AudioQueueRef, AudioQueueBufferRef, UInt32, const AudioStreamPacketDescription *) { return 0; }
extern "C" OSStatus AudioQueueDispose(AudioQueueRef, Boolean) { ++disposals; return 0; }
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
	if (!completion || opens) return 4;
	status = AVAuthorizationStatusAuthorized;
	completion(YES);
	completion = nil;
	{
		raven_darwin::AudioInput granted({48000,1,32,4,true});
		if (!granted.ready() || opens != 1 || buffers != 4 || requests != 1) return 5;
	}
	if (disposals != 1) return 6;
	unsigned long size=0;
	const auto plist=getsectiondata(&_mh_execute_header, "__TEXT", "__info_plist", &size);
	if (!plist || std::string_view(reinterpret_cast<const char *>(plist),size).find("NSMicrophoneUsageDescription") == std::string_view::npos) return 7;
	return std::chrono::steady_clock::now()-start < std::chrono::seconds(5) ? 0 : 8;
}
