#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include "gui_session.h"

void raven_macos_startup(int,char **);

int main(int argc,char **argv)
{
	using Responsible = pid_t (*)(pid_t);
	auto responsible = reinterpret_cast<Responsible>(dlsym(RTLD_DEFAULT,"responsibility_get_pid_responsible_for_pid"));
	if (!responsible || !dlsym(RTLD_DEFAULT,"responsibility_spawnattrs_setdisclaim")) {
		fprintf(stderr,"SKIP: responsibility SPI unavailable\n");
		return 77;
	}
	if (!raven_macos_gui_available()) {
		fprintf(stderr,"SKIP: reexec contract requires a GUI session and WindowServer\n");
		return 77;
	}
	for (int i=1;i<argc;++i) if (const char *value=strstr(argv[i],"-fd=")) fcntl(atoi(value+4),F_SETFD,FD_CLOEXEC);
	if (!getenv("RAVEN_TEST_ORIGINAL_PID")) setenv("RAVEN_TEST_ORIGINAL_PID",std::to_string(getpid()).c_str(),1);
	raven_macos_startup(argc,argv);
	if (responsible(getpid()) != getpid()) {
		fprintf(stderr,"responsibility handoff failed: pid=%d responsible=%d\n",getpid(),responsible(getpid()));
		return 45;
	}
	if (const char *report = getenv("RAVEN_TEST_RESPONSIBILITY_REPORT")) {
		FILE *file = fopen(report,"w");
		if (!file) return 46;
		fprintf(file,"%d %d",getpid(),responsible(getpid())); fclose(file);
	}
	if (const char *report=getenv("RAVEN_TEST_TICKET_REPORT")) {
		FILE *file=fopen(report,"w");
		if (!file) return 44;
		fprintf(file,"%s",getenv("RAVEN_FOREGROUND_TICKET") ?: ""); fclose(file);
	}
	if (atoi(getenv("RAVEN_TEST_ORIGINAL_PID"))!=getpid()) return 40;
	char buffer[8]{};
	if (read(0,buffer,5)!=5 || memcmp(buffer,"stdin",5)) return 41;
	int input=-1,channel=-1,exit_fd=-1;
	for (int i=1;i<argc;++i) {
		if (!strncmp(argv[i],"--inherit-fd=",13)) input=atoi(argv[i]+13);
		if (!strncmp(argv[i],"--handle-channel-fd=",20)) channel=atoi(argv[i]+20);
		if (!strncmp(argv[i],"--exit-code-fd=",15)) exit_fd=atoi(argv[i]+15);
	}
	if (read(input,buffer,5)!=5 || memcmp(buffer,"input",5)) return 42;
	if (write(channel,"channel",7)!=7 || write(exit_fd,"exit",4)!=4) return 43;
	(void)write(1,"stdout",6); (void)write(2,"stderr",6);
	return 37;
}
