#pragma once
#include_next <fcntl.h>

// File seals. macOS has none; adding them succeeds without effect, so storage the runtime seals is guarded only by its own discipline.
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008

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
