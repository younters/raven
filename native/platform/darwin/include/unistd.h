#pragma once
#include_next <unistd.h>
#include <errno.h>
#include <pthread.h>

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

// readlink("/proc/self/exe"): the running executable (see the filesystem shim); "/proc/<pid>/exe": the program of that process.
#define readlink(path, buffer, size) raven_darwin_readlink(path, buffer, size)

// gettid: the calling host thread's id. Only upstream's one-host-thread-per-guest-thread mode asks for it, which the macOS engines leave
// off (threaded_host), so a truncated pthread id is enough.
static inline pid_t gettid(void)
{
	uint64_t id = 0;
	pthread_threadid_np(NULL, &id);
	return (pid_t)id;
}

// copy_file_range: macOS copies no range of one file into another without reading it. The call answers ENOSYS, as a Linux kernel without
// it does, and the caller writes the range itself.
typedef off_t loff_t;
static inline ssize_t copy_file_range(int from, loff_t *from_offset, int to, loff_t *to_offset, size_t length, unsigned int flags)
{
	(void)from; (void)from_offset; (void)to; (void)to_offset; (void)length; (void)flags;
	errno = ENOSYS;
	return -1;
}
