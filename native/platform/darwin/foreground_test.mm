#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

void raven_macos_prepare_identity();
bool raven_macos_claim_foreground();

int main()
{
	setenv("__CFBundleIdentifier","com.foreign.parent",1);
	raven_macos_prepare_identity();
	if (getenv("__CFBundleIdentifier")) throw std::runtime_error("foreign identity retained");
	setenv("__CFBundleIdentifier","org.raven.app.fixture",1);
	setenv("RAVEN_APP_NAME","Fixture",1);
	setenv("RAVEN_APP_ICON","/tmp/fixture.icns",1);
	raven_macos_prepare_identity();
	if (!getenv("__CFBundleIdentifier")) throw std::runtime_error("launcher identity removed");
	char path[]="/tmp/raven-foreground-test-XXXXXX";
	int fd=mkstemp(path);
	if (fd<0) throw std::runtime_error("ticket setup failed");
	setenv("RAVEN_FOREGROUND_TICKET",path,1);
	char value[64];
	const pid_t foreground=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	int length=snprintf(value,sizeof(value),"%d\n0\n",foreground);
	(void)write(fd,value,size_t(length)); close(fd);
	if (!raven_macos_claim_foreground() || raven_macos_claim_foreground()) throw std::runtime_error("ticket not one-use");
	fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);
	length=snprintf(value,sizeof(value),"99999999\n0\n");
	(void)write(fd,value,size_t(length)); close(fd);
	if (raven_macos_claim_foreground()) throw std::runtime_error("expired context activated");
	return 0;
}
