#include <winsock2.h>
int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2,2),&data)) return 1;
	SOCKET a = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP), b = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	int value=1, size=sizeof(value);
	if (a==INVALID_SOCKET || b==INVALID_SOCKET || setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,size)) return 2;
	value=0;
	if (getsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,&size) || value!=1) return 3;
	if (!setsockopt(a,SOL_SOCKET,SO_REUSEADDR,(char *)&value,size) || WSAGetLastError()!=WSAEINVAL) return 4;
	struct sockaddr_in address={0};
	address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
	if (bind(a,(struct sockaddr *)&address,sizeof(address))) return 5;
	size=sizeof(address);
	if (getsockname(a,(struct sockaddr *)&address,&size)) return 6;
	if (!bind(b,(struct sockaddr *)&address,sizeof(address)) || WSAGetLastError()!=WSAEADDRINUSE) return 7;
	if (!setsockopt(a,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&value,sizeof(value)) || WSAGetLastError()!=WSAEINVAL) return 8;
	closesocket(a); closesocket(b); WSACleanup();
	return 0;
}
