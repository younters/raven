#pragma once
#include_next <sys/un.h>
#include <stddef.h>

// Abstract Unix socket names (a sun_path starting with a zero byte) are files in a per-user runtime directory on macOS; bind and
// connect translate them (see compat/sockets.cpp). Linux lists the bound ones in /proc/net/unix; this tells whether a process binds
// the abstract name of `length` bytes.
#ifdef __cplusplus
extern "C" {
#endif
int raven_darwin_unix_binding_lock(int descriptor);
int raven_darwin_adopt_unix_binding(int descriptor, int lock);
int raven_darwin_unix_name_bound(const char *name, size_t length);
#ifdef __cplusplus
}
#endif
