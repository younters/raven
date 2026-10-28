// Apply launcher metadata once, before AppKit registers this process with the Dock.
#import <AppKit/AppKit.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <string>
#include "pe/pe.h"
#include "pe/resources.h"
#include "graphics/icon_file.h"
#include "graphics/icon_bitmap.h"

namespace {
uint16_t word(std::span<const uint8_t> data, size_t at) {
	return at+2<=data.size() ? uint16_t(data[at] | (uint16_t(data[at+1])<<8)) : 0;
}

NSString *version_string(std::span<const uint8_t> data, size_t start, NSString *wanted)
{
	if (start+6>data.size()) return nil;
	const size_t end = std::min(data.size(), start+word(data,start));
	if (end<=start+6) return nil;
	size_t at=start+6;
	while (at+2<=end && word(data,at)) at+=2;
	if (at+2>end) return nil;
	NSString *key = [[NSString alloc] initWithBytes:data.data()+start+6 length:at-(start+6) encoding:NSUTF16LittleEndianStringEncoding];
	at=(at+2+3)&~size_t(3);
	const size_t value_length=size_t(word(data,start+2))*(word(data,start+4)==1 ? 2 : 1);
	if (key && [key isEqualToString:wanted] && value_length>=2 && at+value_length<=end) {
		NSString *value=[[NSString alloc] initWithBytes:data.data()+at length:value_length-2 encoding:NSUTF16LittleEndianStringEncoding];
		if (value.length) return value;
	}
	at=(std::min(end,at+value_length)+3)&~size_t(3);
	while (at+6<=end) {
		if (NSString *found=version_string(data,at,wanted)) return found;
		const size_t step=word(data,at);
		if (step<6) break;
		at=(at+step+3)&~size_t(3);
	}
	return nil;
}

NSImage *program_icon(std::span<const uint8_t> file)
{
	raven::IconFile icons(file);
	if (!icons.count()) return nil;
	auto found=icons.image(0,256,256,{256,256,32});
	if (!found) return nil;
	auto bytes=found->bytes;
	if (bytes.size()>=8 && bytes[0]==0x89 && bytes[1]=='P')
		return [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]];
	auto bitmap=raven::decode_icon_dib(bytes);
	for (size_t i=0;i<bitmap.and_mask.size();++i)
		bitmap.color.rgba[i*4+3]=bitmap.and_mask[i] ? 0 : bitmap.has_alpha ? bitmap.color.rgba[i*4+3] : 255;
	CGDataProviderRef provider=CGDataProviderCreateWithData(nullptr,bitmap.color.rgba.data(),bitmap.color.rgba.size(),nullptr);
	CGColorSpaceRef colors=CGColorSpaceCreateDeviceRGB();
	CGImageRef image=CGImageCreate(bitmap.color.width,bitmap.color.height,8,32,bitmap.color.width*4,colors,
		kCGImageAlphaLast|kCGBitmapByteOrderDefault,provider,nullptr,false,kCGRenderingIntentDefault);
	NSImage *result=image ? [[NSImage alloc] initWithCGImage:image size:NSZeroSize] : nil;
	if (image) CGImageRelease(image);
	CGColorSpaceRelease(colors);
	CGDataProviderRelease(provider);
	return result;
}
}

void raven_macos_program_identity()
{
	if (std::getenv("RAVEN_APP_NAME") && std::getenv("RAVEN_APP_ICON")) return;
	const char *path=std::getenv("RAVEN_PROGRAM_IMAGE");
	if (!path) return;
	try {
		std::ifstream stream(path,std::ios::binary);
		if (!stream) return;
		std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream),{}};
		raven::PEImage image(file);
		NSString *name=nil, *product=nil;
		for (const auto &resource:image.resources()) {
			if (resource.type!=raven::ResourceKey(uint32_t(16))) continue;
			const auto data=image.file_span(resource.data_address,resource.size);
			name=version_string(data,0,@"FileDescription");
			product=version_string(data,0,@"ProductName");
			if (name || product) break;
		}
		NSString *fallback=[NSString stringWithUTF8String:path].lastPathComponent.stringByDeletingPathExtension;
		NSProcessInfo.processInfo.processName=name ?: product ?: fallback;
		if (NSImage *icon=program_icon(file)) NSApp.applicationIconImage=icon;
	} catch (...) {
		NSString *fallback=[NSString stringWithUTF8String:path].lastPathComponent.stringByDeletingPathExtension;
		NSProcessInfo.processInfo.processName=fallback;
	}
}

void raven_macos_prepare_identity()
{
	const char *bundle = std::getenv("__CFBundleIdentifier");
	const char *name = std::getenv("RAVEN_APP_NAME");
	const char *icon = std::getenv("RAVEN_APP_ICON");
	if (bundle && !(std::strncmp(bundle, "org.raven.", 10) == 0 && name && *name && icon && *icon)) {
		unsetenv("__CFBundleIdentifier");
		unsetenv("RAVEN_APP_NAME");
		unsetenv("RAVEN_APP_ICON");
	}
}

void raven_macos_prepare_foreground()
{
	if (std::getenv("RAVEN_FOREGROUND_TICKET") || std::getenv("RAVEN_FOREGROUND_CHILD")) return;
	// This is a host activation ticket, shared by the initial host and its descendants.
	char path[] = "/tmp/raven-foreground-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return;
	const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	char value[64];
	const int length = snprintf(value, sizeof(value), "%d\n0\n", front);
	if (length > 0) (void)write(fd, value, size_t(length));
	close(fd);
	setenv("RAVEN_FOREGROUND_TICKET", path, 1);
}

bool raven_macos_claim_foreground()
{
	const char *path = std::getenv("RAVEN_FOREGROUND_TICKET");
	if (!path) return false;
	int fd = open(path, O_RDWR);
	if (fd < 0) return false;
	if (flock(fd, LOCK_EX) != 0) { close(fd); return false; }
	char contents[64]{};
	const ssize_t count = pread(fd, contents, sizeof(contents)-1, 0);
	int origin = 0, consumed = 1;
	if (count > 0) (void)sscanf(contents, "%d\n%d", &origin, &consumed);
	const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	const bool allowed = !consumed && origin > 0 && (front == origin || front == getpid());
	if (!consumed) {
		const int length = snprintf(contents, sizeof(contents), "%d\n1\n", origin);
		if (length > 0) (void)pwrite(fd, contents, size_t(length), 0);
		(void)unlink(path);
	}
	flock(fd, LOCK_UN);
	close(fd);
	return allowed;
}

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
