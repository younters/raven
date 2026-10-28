// Apply launcher metadata once, before AppKit registers this process with the Dock.
#import <AppKit/AppKit.h>
#include <algorithm>
#include <crt_externs.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

#include "graphics/icon_bitmap.h"
#include "graphics/icon_file.h"
#include "pe/pe.h"
#include "pe/resources.h"

namespace {
int foreground_lease = -1;
std::string foreground_lease_path;

void cleanup_foreground_ticket()
{
	if (foreground_lease < 0) return;
	close(foreground_lease);
	foreground_lease = -1;
	int fd = open(foreground_lease_path.c_str(),O_RDWR|O_CLOEXEC);
	if (fd < 0) return;
	if (!flock(fd,LOCK_EX|LOCK_NB)) {
		const auto ticket = foreground_lease_path.substr(0,foreground_lease_path.size()-6);
		unlink(ticket.c_str());
		unlink(foreground_lease_path.c_str());
	}
	close(fd);
}

void retain_foreground_ticket()
{
	if (foreground_lease >= 0) return;
	const char *inherited = getenv("RAVEN_FOREGROUND_LEASE_FD");
	const char *lease_path = getenv("RAVEN_FOREGROUND_LEASE_PATH");
	if (inherited && lease_path && fcntl(atoi(inherited),F_GETFD)>=0) {
		foreground_lease = atoi(inherited);
		foreground_lease_path = lease_path;
		fcntl(foreground_lease,F_SETFD,FD_CLOEXEC);
	} else if (const char *ticket = getenv("RAVEN_FOREGROUND_TICKET")) {
		foreground_lease_path = std::string(ticket)+".lease";
		int fd = open(foreground_lease_path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
		if (fd < 0) return;
		foreground_lease = fcntl(fd,F_DUPFD_CLOEXEC,1024);
		if (foreground_lease >= 0) close(fd);
		else foreground_lease = fd;
		if (flock(foreground_lease,LOCK_SH)) { close(foreground_lease); foreground_lease = -1; return; }
		setenv("RAVEN_FOREGROUND_LEASE_FD",std::to_string(foreground_lease).c_str(),1);
		setenv("RAVEN_FOREGROUND_LEASE_PATH",foreground_lease_path.c_str(),1);
	}
	if (foreground_lease >= 0) std::atexit(cleanup_foreground_ticket);
}

void update_registered_name(NSString *name)
{
	// WebKit uses this optional LaunchServices SPI for names of independently registered helper processes.
	using Current = CFTypeRef (*)();
	using Set = int32_t (*)(int32_t,CFTypeRef,CFStringRef,CFTypeRef,CFDictionaryRef *);
	auto current = reinterpret_cast<Current>(dlsym(RTLD_DEFAULT,"_LSGetCurrentApplicationASN"));
	auto set = reinterpret_cast<Set>(dlsym(RTLD_DEFAULT,"_LSSetApplicationInformationItem"));
	auto key = reinterpret_cast<CFStringRef *>(dlsym(RTLD_DEFAULT,"_kLSDisplayNameKey"));
	if (current && set && key) {
		(void)set(-2,current(),*key,(__bridge CFStringRef)name,nullptr);
	}
}
uint16_t word(std::span<const uint8_t> data, size_t at)
{
	return at+2<=data.size() ? uint16_t(data[at] | (uint16_t(data[at+1])<<8)) : 0;
}

NSString *version_string(std::span<const uint8_t> data, size_t start, NSString *wanted, unsigned depth = 0)
{
	if (depth >= 32) return nil;
	if (start+6>data.size()) return nil;
	const size_t end = std::min(data.size(), start+word(data,start));
	if (end<=start+6) return nil;
	size_t at = start+6;
	while (at+2<=end && word(data,at)) at+=2;
	if (at+2>end) return nil;
	NSString *key = [[NSString alloc] initWithBytes:data.data()+start+6 length:at-(start+6) encoding:NSUTF16LittleEndianStringEncoding];
	at = (at+2+3)&~size_t(3);
	const size_t value_length = size_t(word(data,start+2))*(word(data,start+4)==1 ? 2 : 1);
	if (key && [key isEqualToString:wanted] && value_length>=2 && at+value_length<=end) {
		NSString *value = [[NSString alloc] initWithBytes:data.data()+at length:value_length-2 encoding:NSUTF16LittleEndianStringEncoding];
		if (value.length) return value;
	}
	at = (std::min(end,at+value_length)+3)&~size_t(3);
	while (at+6<=end) {
		if (NSString *found = version_string(data,at,wanted,depth+1)) return found;
		const size_t step = word(data,at);
		if (step<6) break;
		at = (at+step+3)&~size_t(3);
	}
	return nil;
}

NSImage *program_icon(std::span<const uint8_t> file)
{
	raven::IconFile icons(file);
	if (!icons.count()) return nil;
	auto found = icons.image(0,256,256,{256,256,32});
	if (!found) return nil;
	auto bytes = found->bytes;
	if (bytes.size()>=8 && bytes[0]==0x89 && bytes[1]=='P')
		return [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]];
	auto bitmap = raven::decode_icon_dib(bytes);
	for (size_t i = 0;i<bitmap.and_mask.size();++i)
		bitmap.color.rgba[i*4+3] = bitmap.and_mask[i] ? 0 : bitmap.has_alpha ? bitmap.color.rgba[i*4+3] : 255;
	CFDataRef data = CFDataCreate(kCFAllocatorDefault,bitmap.color.rgba.data(),CFIndex(bitmap.color.rgba.size()));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CFRelease(data);
	CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
	CGImageRef image = CGImageCreate(bitmap.color.width,bitmap.color.height,8,32,bitmap.color.width*4,colors,
		CGBitmapInfo(kCGImageAlphaLast)|kCGBitmapByteOrderDefault,provider,nullptr,false,kCGRenderingIntentDefault);
	NSImage *result = image ? [[NSImage alloc] initWithCGImage:image size:NSZeroSize] : nil;
	if (image) CGImageRelease(image);
	CGColorSpaceRelease(colors);
	CGDataProviderRelease(provider);
	return result;
}
}

void raven_macos_program_identity()
{
	if (std::getenv("RAVEN_APP_NAME") && std::getenv("RAVEN_APP_ICON")) return;
	const char *path = std::getenv("RAVEN_PROGRAM_IMAGE");
	if (!path) return;
	try {
		std::ifstream stream(path,std::ios::binary);
		if (!stream) return;
		std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream),{}};
		raven::PEImage image(file);
		NSString *name = nil, *product = nil;
		for (const auto &resource:image.resources()) {
			if (resource.type!=raven::ResourceKey(uint32_t(16))) continue;
			const auto data = image.file_span(resource.data_address,resource.size);
			name = version_string(data,0,@"FileDescription");
			product = version_string(data,0,@"ProductName");
			if (name || product) break;
		}
		NSString *fallback = [NSString stringWithUTF8String:path].lastPathComponent.stringByDeletingPathExtension;
		NSProcessInfo.processInfo.processName = name ?: product ?: fallback;
		update_registered_name(name ?: product ?: fallback);
		if (NSImage *icon = program_icon(file)) NSApp.applicationIconImage = icon;
	} catch (...) {
		NSString *fallback = [NSString stringWithUTF8String:path].lastPathComponent.stringByDeletingPathExtension;
		NSProcessInfo.processInfo.processName = fallback;
		update_registered_name(fallback);
	}
}

void raven_macos_prepare_identity()
{
	const char *bundle = std::getenv("__CFBundleIdentifier");
	const char *name = std::getenv("RAVEN_APP_NAME");
	const char *icon = std::getenv("RAVEN_APP_ICON");
	if (!(bundle && std::strncmp(bundle, "org.raven.", 10) == 0 && name && *name && icon && *icon)) {
		unsetenv("__CFBundleIdentifier");
		unsetenv("RAVEN_APP_NAME");
		unsetenv("RAVEN_APP_ICON");
	}
}

void raven_macos_disclaim_foreign_identity(char **argv)
{
	using Responsible = pid_t (*)(pid_t);
	auto responsible = reinterpret_cast<Responsible>(dlsym(RTLD_DEFAULT,"responsibility_get_pid_responsible_for_pid"));
	if (std::getenv("RAVEN_TRACE_COCOA") && responsible)
		fprintf(stderr,"identity: startup pid=%d responsible=%d checked=%s\n",getpid(),responsible(getpid()),getenv("RAVEN_RESPONSIBILITY_CHECKED") ?: "no");
	if (std::getenv("RAVEN_RESPONSIBILITY_CHECKED") || std::getenv("RAVEN_FOREGROUND_CHILD") ||
		(std::getenv("RAVEN_APP_NAME") && std::getenv("RAVEN_APP_ICON"))) return;
	using Disclaim = int (*)(posix_spawnattr_t *, int);
	auto disclaim = reinterpret_cast<Disclaim>(dlsym(RTLD_DEFAULT,"responsibility_spawnattrs_setdisclaim"));
	if (std::getenv("RAVEN_TRACE_COCOA") && responsible)
		fprintf(stderr,"identity: pid=%d responsible=%d checked=%s\n",getpid(),responsible(getpid()),getenv("RAVEN_RESPONSIBILITY_CHECKED") ?: "no");
	if (!disclaim || (responsible && responsible(getpid())==getpid())) return;
	posix_spawnattr_t attributes;
	posix_spawn_file_actions_t actions;
	if (posix_spawnattr_init(&attributes)) return;
	posix_spawn_file_actions_init(&actions);
	// A same-PID exec preserves status plumbing and ordinary inherited descriptors. Explicit descriptors may already be CLOEXEC.
	for (char **arg = argv+1; *arg; ++arg) {
		for (const std::string_view option : {"--environment-fd=", "--suspended-fd=", "--inherit-fd=", "--process-token-fd=",
			"--handle-channel-fd=", "--diagnostics-fd=", "--exit-code-fd="}) {
			if (std::string_view(*arg).starts_with(option)) {
				const int fd = atoi(*arg+option.size());
				if (fd>=0 && fcntl(fd,F_GETFD)>=0) posix_spawn_file_actions_addinherit_np(&actions,fd);
				break;
			}
		}
	}
	setenv("RAVEN_RESPONSIBILITY_CHECKED","1",1);
	const int disclaim_status = disclaim(&attributes,1);
	if (std::getenv("RAVEN_TRACE_COCOA")) fprintf(stderr,"identity: disclaim status=%d\n",disclaim_status);
	if (!disclaim_status && !posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETEXEC)) {
		pid_t pid;
		const int result = posix_spawnp(&pid,argv[0],&actions,&attributes,argv,*_NSGetEnviron());

		if (std::getenv("RAVEN_TRACE_COCOA")) fprintf(stderr,"identity: reexec failed %d\n",result);
	}
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
}

void raven_macos_foreground_changed(pid_t front)
{
	const char *path = getenv("RAVEN_FOREGROUND_TICKET");
	if (!path) return;
	int fd = open(path,O_RDWR);
	if (fd<0) return;
	if (!flock(fd,LOCK_EX)) {
		char value[64]{};
		int origin = 0,consumed = 1,root = 0;
		if (pread(fd,value,sizeof(value)-1,0)>0) sscanf(value,"%d\n%d\n%d",&origin,&consumed,&root);
		if (front!=origin && front!=root && front!=getpid()) {
			const int size = snprintf(value,sizeof(value),"%d\n%d\n%d\n1\n",origin,consumed,root);
			(void)pwrite(fd,value,size_t(size),0);
		}
		flock(fd,LOCK_UN);
	}
	close(fd);
}

void raven_macos_prepare_foreground()
{
	if (!std::getenv("RAVEN_APP_NAME") && std::getenv("RAVEN_RESPONSIBILITY_CHECKED")) {
		using Register = int32_t (*)(CFDictionaryRef,void *);
		auto register_app = reinterpret_cast<Register>(dlsym(RTLD_DEFAULT,"_RegisterApplication"));
		if (register_app) {
			NSDictionary *info = @{@"CFBundleIdentifier":@"org.raven.runtime", @"CFBundleName":@"Raven", @"LSUIElement":@YES,
				@"LSDisplayName":@"Raven", @"CFBundleVersion":@"1", @"LSBundlePath":NSBundle.mainBundle.bundlePath,
				@"LSExecutablePath":NSBundle.mainBundle.executablePath};
			int32_t result = register_app((__bridge CFDictionaryRef)info,nullptr);
			if (getenv("RAVEN_TRACE_COCOA")) fprintf(stderr,"identity: register=%d bundle=%s\n",result,NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "none");
		}
	}
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
	if (!std::getenv("RAVEN_FOREGROUND_TICKET") && !std::getenv("RAVEN_FOREGROUND_CHILD")) {
	// This is a host activation ticket, shared by the initial host and its descendants.
	char path[] = "/tmp/raven-foreground-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return;
	const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	char value[64];
	const int length = snprintf(value, sizeof(value), "%d\n0\n%d\n", front,getpid());
	if (length > 0) (void)write(fd, value, size_t(length));
	close(fd);
	setenv("RAVEN_FOREGROUND_TICKET", path, 1);
	}
	retain_foreground_ticket();
	static id observer = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
		object:nil queue:nil usingBlock:^(NSNotification *note) {
			NSRunningApplication *app = note.userInfo[NSWorkspaceApplicationKey];
			raven_macos_foreground_changed(app.processIdentifier);
		}];
	(void)observer;
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
	int origin = 0, consumed = 1, root = 0, expired = 0;
	if (count > 0) (void)sscanf(contents, "%d\n%d\n%d\n%d", &origin, &consumed,&root,&expired);
	const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	const bool allowed = !consumed && origin > 0 && (front == getpid() || (!expired && (front == origin || front == root)));
	if (allowed) {
		const int length = snprintf(contents, sizeof(contents), "%d\n1\n%d\n", origin,root);
		if (length > 0) (void)pwrite(fd, contents, size_t(length), 0);
		(void)unlink(path);
	}
	flock(fd, LOCK_UN);
	close(fd);
	if (getenv("RAVEN_TRACE_COCOA")) fprintf(stderr,"foreground: pid=%d origin=%d root=%d front=%d consumed=%d allowed=%d\n",getpid(),origin,root,front,consumed,allowed);
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

void raven_macos_startup(int argc,char **argv)
{
	raven_macos_prepare_identity();
	if (argc<4 || std::string_view(argv[1])!="--run-process") return;
	bool desktop = false;
	int image = 3;
	for (;image<argc && std::string_view(argv[image]).starts_with("--");++image) {
		if (std::string_view(argv[image])=="--desktop=macos") desktop = true;
		if (std::string_view(argv[image])=="--") break;
	}
	if (!desktop) return;
	if (image<argc) setenv("RAVEN_PROGRAM_IMAGE",std::filesystem::absolute(argv[image]).string().c_str(),1);
	raven_macos_disclaim_foreign_identity(argv);
	raven_macos_prepare_foreground();
}
