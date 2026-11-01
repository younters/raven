// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
struct raven_mtld3d_callbacks {
    uint32_t version, size;
    void *(*create)(uint64_t, void *, uint32_t, uint32_t);
    void (*release)(void *);
    int32_t (*present)(void *, void *, double);
};
#ifdef __cplusplus
static_assert(sizeof(raven_mtld3d_callbacks) == 32);
static_assert(offsetof(raven_mtld3d_callbacks, create) == 8);
static_assert(offsetof(raven_mtld3d_callbacks, release) == 16);
static_assert(offsetof(raven_mtld3d_callbacks, present) == 24);
#endif
