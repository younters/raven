// Apply launcher metadata once, before AppKit registers this process with the Dock.
#import <AppKit/AppKit.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
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
