// inotify on macOS, as far as the runtime uses it: a descriptor that becomes readable when entries of a watched directory are created,
// renamed or removed. A dispatch vnode source on the directory writes to it; the bytes read are not inotify_event records.
#pragma once
#include <fcntl.h>
#include <stdint.h>

#define IN_ACCESS 0x00000001
#define IN_MODIFY 0x00000002
#define IN_ATTRIB 0x00000004
#define IN_CLOSE_WRITE 0x00000008
#define IN_MOVED_FROM 0x00000040
#define IN_MOVED_TO 0x00000080
#define IN_CREATE 0x00000100
#define IN_DELETE 0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF 0x00000800
#define IN_ONLYDIR 0x01000000
#define IN_CLOEXEC O_CLOEXEC
#define IN_NONBLOCK O_NONBLOCK

struct inotify_event {
	int wd;
	uint32_t mask, cookie, len;
	char name[];
};

#ifdef __cplusplus
extern "C" {
#endif
int inotify_init1(int flags);
int inotify_add_watch(int descriptor, const char *path, uint32_t mask);
int inotify_rm_watch(int descriptor, int watch);
#ifdef __cplusplus
}
#endif
