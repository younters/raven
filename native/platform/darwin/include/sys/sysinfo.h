// sysinfo(2), answered from the Mach host statistics and sysctl.
#pragma once

struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
};

#ifdef __cplusplus
extern "C" {
#endif
int sysinfo(struct sysinfo *info);
#ifdef __cplusplus
}
#endif
