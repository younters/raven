#pragma once
#include_next <fcntl.h>

// File seals. macOS has none; adding them succeeds without effect, so storage the runtime seals is guarded only by its own discipline.
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008

// open("/proc/self/fd/N") on Linux opens the file again, as a new open file description; /dev/fd/N on macOS duplicates the
// descriptor instead. This opens the file by the path the descriptor has, provided that is still the same file.
#ifdef __cplusplus
extern "C" {
#endif
int raven_darwin_reopen(int descriptor, int flags);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
inline int fcntl(int descriptor, int command, int argument)
{
	if (command == F_ADD_SEALS)
		return 0;
	if (command == F_GET_SEALS)
		return 0;
	int (*const system)(int, int, ...) = ::fcntl;
	return system(descriptor, command, argument);
}
#endif
