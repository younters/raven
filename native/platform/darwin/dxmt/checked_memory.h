// SPDX-License-Identifier: MIT
#pragma once
#include "../unix_memory.h"
#ifdef __cplusplus
extern "C" {
#endif
void raven_unix_set_memory_check(raven_unix_check_range);
void *raven_unix_checked(const void *, uint64_t count, uint64_t stride, uint32_t write);
static inline void *raven_unix_optional(const void *p, uint64_t count, uint64_t stride, uint32_t write) {
    return p ? raven_unix_checked(p, count, stride, write) : NULL;
}
void *raven_unix_copy(void *, const void *, size_t);
void raven_unix_executable(const void *);
const char *raven_unix_string(const void *);
#ifdef __cplusplus
}
#endif
