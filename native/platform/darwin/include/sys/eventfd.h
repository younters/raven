// A counter with a level-triggered FIFO token. Host read/write/close are dispatched by compat/eventfd.cpp.
#pragma once
#include <fcntl.h>
#include <stdint.h>

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

typedef uint64_t eventfd_t;

#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned int initial, int flags);
int eventfd_read(int descriptor, eventfd_t *value);
int eventfd_write(int descriptor, eventfd_t value);
#ifdef __cplusplus
}
#endif
