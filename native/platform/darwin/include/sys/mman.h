#pragma once
#include_next <sys/mman.h>
#include <fcntl.h>

// memfd_create: an unlinked file in the user's temporary directory, which grows with ftruncate, takes pwrite and maps shared like a
// memfd (POSIX shared memory objects on macOS do neither).
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

#ifdef __cplusplus
extern "C" {
#endif
int memfd_create(const char *name, unsigned int flags);
#ifdef __cplusplus
}
#endif
