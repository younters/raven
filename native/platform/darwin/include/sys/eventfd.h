// eventfd on macOS: a FIFO opened for reading and writing through one descriptor. Each write leaves its eight bytes and a read takes
// what is there, so the descriptor stays readable until it is drained: the way the runtime uses its wake descriptors. Counter
// semantics (reading the sum) are not provided.
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
#ifdef __cplusplus
}
#endif
