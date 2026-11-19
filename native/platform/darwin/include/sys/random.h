#pragma once
#include_next <sys/random.h>
#include <sys/types.h>

#define GRND_NONBLOCK 1
#define GRND_RANDOM 2

#ifdef __cplusplus
extern "C" {
#endif
// Never short, never blocks: arc4random_buf.
ssize_t getrandom(void *buffer, size_t length, unsigned int flags);
#ifdef __cplusplus
}
#endif
