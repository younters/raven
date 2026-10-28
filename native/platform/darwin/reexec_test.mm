#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

void raven_macos_startup(int,char **);

int main(int argc,char **argv)
{
	for (int i=1;i<argc;++i) if (const char *value=strstr(argv[i],"-fd=")) fcntl(atoi(value+4),F_SETFD,FD_CLOEXEC);
	if (!getenv("RAVEN_TEST_ORIGINAL_PID")) setenv("RAVEN_TEST_ORIGINAL_PID",std::to_string(getpid()).c_str(),1);
	raven_macos_startup(argc,argv);
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
