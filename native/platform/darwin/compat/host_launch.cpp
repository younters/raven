#include "raven_darwin_launch.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <libproc.h>
#include <sys/event.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
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

void wait_process_group(pid_t group)
{
	const int queue=kqueue();
	for (;;) {
		std::vector<pid_t> members(256);
		int bytes=proc_listpgrppids(group,members.data(),int(members.size()*sizeof(pid_t)));
		while (bytes>=int(members.size()*sizeof(pid_t))) {
			members.resize(members.size()*2);
			bytes=proc_listpgrppids(group,members.data(),int(members.size()*sizeof(pid_t)));
		}
		members.resize(bytes>0 ? size_t(bytes)/sizeof(pid_t) : 0);
		// A zombie has ended; its parent reaps it.
		std::erase_if(members,[](pid_t member) {
			proc_bsdinfo info{};
			return member<=0 || proc_pidinfo(member,PROC_PIDTBSDINFO,0,&info,sizeof info)!=int(sizeof info) || info.pbi_status==SZOMB;
		});
		if (members.empty()) break;
		// Woken by the first member to end, or after a while, for members that joined meanwhile.
		if (queue<0) { usleep(200000); continue; }
		std::vector<struct kevent> watched;
		for (const pid_t member : members) {
			struct kevent change;
			EV_SET(&change,uintptr_t(member),EVFILT_PROC,EV_ADD | EV_ONESHOT,NOTE_EXIT,0,nullptr);
			watched.push_back(change);
		}
		struct kevent fired;
		const timespec timeout{0,500000000};
		kevent(queue,watched.data(),int(watched.size()),&fired,1,&timeout);
	}
	if (queue>=0) close(queue);
}
}
