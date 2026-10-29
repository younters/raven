#include <winsock2.h>
#include <iphlpapi.h>
int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2,2),&data)) return 1;
	SOCKET a = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP), b = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	int value=0, size=sizeof(value);
	if (getsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,&size) || value) return 9;
	value=1;
	if (a==INVALID_SOCKET || b==INVALID_SOCKET || setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,size)) return 2;
	value=0;
	if (getsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,&size) || value!=1) return 3;
	size=1;
	if (!getsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,&size) || WSAGetLastError()!=WSAEFAULT) return 10;
	size=sizeof(value);
	value=0;
	if (setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,size)) return 11;
	value=1;
	if (setsockopt(a,SOL_SOCKET,SO_REUSEADDR,(char *)&value,size)) return 12;
	if (!setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,size) || WSAGetLastError()!=WSAEINVAL) return 13;
	value=0;
	if (setsockopt(a,SOL_SOCKET,SO_REUSEADDR,(char *)&value,size)) return 14;
	value=1;
	if (setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,size)) return 15;
	value=0;
	if (setsockopt(a,SOL_SOCKET,SO_REUSEADDR,(char *)&value,size)) return 4;
	value=1;
	if (!setsockopt(a,SOL_SOCKET,SO_REUSEADDR,(char *)&value,size) || WSAGetLastError()!=WSAEINVAL) return 16;
	value=0;
	struct sockaddr_in address={0};
	address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
	if (bind(a,(struct sockaddr *)&address,sizeof(address))) return 5;
	size=sizeof(address);
	if (getsockname(a,(struct sockaddr *)&address,&size)) return 6;
	if (!bind(b,(struct sockaddr *)&address,sizeof(address)) || WSAGetLastError()!=WSAEADDRINUSE) return 7;
	value=1;
	if (setsockopt(b,SOL_SOCKET,SO_REUSEADDR,(char *)&value,sizeof(value))) return 17;
	// Documented Darwin deviation: BSD returns INUSE rather than Windows ACCESS
	// for a reuse-enabled contender. The exclusive port must still be protected.
	if (!bind(b,(struct sockaddr *)&address,sizeof(address)) || WSAGetLastError()!=WSAEADDRINUSE) return 18;
	IP_ADAPTER_INFO adapters[16];
	ULONG bytes=sizeof(adapters);
	if (GetAdaptersInfo(adapters, &bytes)) return 19;
	unsigned tested=0;
	for (IP_ADAPTER_INFO *adapter=adapters; adapter && !tested; adapter=adapter->Next)
		for (IP_ADDR_STRING *ip=&adapter->IpAddressList; ip; ip=ip->Next) {
			ULONG host=inet_addr(ip->IpAddress.String);
			if (!host || host==INADDR_NONE || host==htonl(INADDR_LOOPBACK)) continue;
			struct sockaddr_in other=address;
			other.sin_addr.s_addr=host;
			if (bind(b,(struct sockaddr *)&other,sizeof(other))) return 20;
			tested=1; break;
		}
	// Hosts with only loopback cannot exercise a second interface.
	if (!tested) OutputDebugStringA("socket_exclusive: no second IPv4 interface available");
	value=0;
	if (!setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,sizeof(value)) || WSAGetLastError()!=WSAEINVAL) return 8;
	closesocket(a); closesocket(b); WSACleanup();
	return 0;
}
