#pragma once
#include_next <sys/ioctl.h>
#include <net/if.h>

// SIOCGIFHWADDR: macOS reports an interface's hardware address as its AF_LINK address instead, which this answers from.
#define SIOCGIFHWADDR 0x7f02
#define ifr_hwaddr ifr_addr

#ifdef __cplusplus
extern "C" int raven_darwin_hardware_address(struct ifreq *request);

inline int ioctl(int descriptor, unsigned long request, struct ifreq *interface)
{
	if (request == SIOCGIFHWADDR)
		return raven_darwin_hardware_address(interface);
	int (*const system)(int, unsigned long, ...) = ::ioctl;
	return system(descriptor, request, interface);
}
#endif
