#include "raven_darwin_launch.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
namespace raven_darwin {
void child_launch_metadata(std::vector<std::string> &environment,const std::string &image,posix_spawn_file_actions_t *actions)
{
	if (actions) if (const char *lease=getenv("RAVEN_FOREGROUND_LEASE_FD")) {
		const int fd=atoi(lease);
		if (fd>=0 && fcntl(fd,F_GETFD)>=0) posix_spawn_file_actions_addinherit_np(actions,fd);
	}
	std::erase_if(environment,[](const auto &value) { return value.starts_with("RAVEN_FOREGROUND_TICKET=") ||
		value.starts_with("RAVEN_FOREGROUND_CHILD=") || value.starts_with("RAVEN_PROGRAM_IMAGE="); });
	environment.push_back("RAVEN_FOREGROUND_CHILD=1");
	environment.push_back("RAVEN_PROGRAM_IMAGE="+image);
	if (const char *ticket=getenv("RAVEN_FOREGROUND_TICKET")) {
		if (int fd=open(ticket,O_RDONLY); fd>=0) {
			char value[64]{};
			int origin=0,consumed=1;
			if (pread(fd,value,sizeof(value)-1,0)>0 && sscanf(value,"%d\n%d",&origin,&consumed)==2 && !consumed)
				environment.push_back(std::string("RAVEN_FOREGROUND_TICKET=")+ticket);
			close(fd);
		}
	}
}
}
