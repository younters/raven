#include "raven_darwin_launch.h"
#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

int main()
{
	char path[]="/tmp/raven-child-metadata-XXXXXX";
	int fd=mkstemp(path);
	if (fd<0) throw std::runtime_error("cannot create ticket fixture");
	setenv("RAVEN_FOREGROUND_TICKET",path,1);
	(void)write(fd,"123\n0\n456\n",10);
	const std::vector<std::string> original{"RAVEN_FOREGROUND_CHILD=old","RAVEN_FOREGROUND_TICKET=old","RAVEN_PROGRAM_IMAGE=parent.exe",
		"RAVEN_APP_NAME=Firefox","RAVEN_APP_ICON=Firefox.icns","__CFBundleIdentifier=org.raven.app.fixture","RAVEN_DESKTOP_DPI=144"};
	auto environment=original;
	raven_darwin::child_launch_metadata(environment,"child.exe");
	auto has=[&](const std::string &value) { return std::find(environment.begin(),environment.end(),value)!=environment.end(); };
	if (environment.size()!=7 || !has("RAVEN_FOREGROUND_CHILD=1") || !has("RAVEN_PROGRAM_IMAGE=child.exe") ||
		!has(std::string("RAVEN_FOREGROUND_TICKET=")+path) || !has(original[3]) || !has(original[4]) || !has(original[5]) || !has(original[6]))
		throw std::runtime_error("child metadata did not replace transient fields and preserve launcher/DPI fields");
	(void)pwrite(fd,"123\n1\n456\n",10,0);
	environment=original;
	raven_darwin::child_launch_metadata(environment,"later-helper.exe");
	if (std::any_of(environment.begin(),environment.end(),[](const auto &value) { return value.starts_with("RAVEN_FOREGROUND_TICKET="); }))
		throw std::runtime_error("consumed foreground right reached a later helper");
	close(fd); unlink(path);
}
