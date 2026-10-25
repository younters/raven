#pragma once
#include_next <sys/socket.h>
#include <sys/types.h>

// Socket type flags, applied after creation. Every socket the runtime makes also gets SO_NOSIGPIPE, which MSG_NOSIGNAL stands for.
#define SOCK_NONBLOCK 0x20000000
#define SOCK_CLOEXEC 0x10000000
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Peer credentials through LOCAL_PEERPID and getpeereid.
#define SO_PEERCRED 0x7f01
struct ucred {
	pid_t pid;
	uid_t uid;
	gid_t gid;
};

#ifdef __cplusplus
extern "C" {
#endif
int raven_darwin_socket(int domain, int type, int protocol);
int raven_darwin_socketpair(int domain, int type, int protocol, int descriptors[2]);
int accept4(int descriptor, struct sockaddr *address, socklen_t *length, int flags);
int raven_darwin_peer_credentials(int descriptor, struct ucred *credentials, socklen_t *length);
#ifdef __cplusplus
}
inline int getsockopt(int descriptor, int level, int name, ucred *credentials, socklen_t *length)
{
	if (level == SOL_SOCKET && name == SO_PEERCRED)
		return raven_darwin_peer_credentials(descriptor, credentials, length);
	return ::getsockopt(descriptor, level, name, static_cast<void *>(credentials), length);
}
#endif

#define socket(domain, type, protocol) raven_darwin_socket(domain, type, protocol)
#define socketpair(domain, type, protocol, descriptors) raven_darwin_socketpair(domain, type, protocol, descriptors)
