#pragma once
#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif
int pipe2(int descriptors[2], int flags);
int dup3(int from, int to, int flags);
long raven_darwin_sysconf(int name);
ssize_t raven_darwin_readlink(const char *path, char *buffer, size_t size);
#ifdef __cplusplus
}
#endif

// The runtime's page size is the Windows one, which on Linux is also the host's. Apple silicon hosts have 16 KiB pages while the
// Rosetta 2 guest, where Windows memory lives, has 4 KiB ones: sysconf reports the guest's.
#define sysconf(name) raven_darwin_sysconf(name)

// readlink("/proc/self/exe"): the running executable (see the filesystem shim).
#define readlink(path, buffer, size) raven_darwin_readlink(path, buffer, size)
