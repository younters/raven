// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
// Compile the same offset contract into the adapter and both pinned consumers.
#define RAVEN_MACDRV_TABLE(T, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9) \
    _Static_assert(offsetof(T, f0) == 0, "macdrv TABLE slot 0"); \
    _Static_assert(offsetof(T, f1) == 8, "macdrv TABLE slot 1"); \
    _Static_assert(offsetof(T, f2) == 16, "macdrv TABLE slot 2"); \
    _Static_assert(offsetof(T, f3) == 24, "macdrv TABLE slot 3"); \
    _Static_assert(offsetof(T, f4) == 32, "macdrv TABLE slot 4"); \
    _Static_assert(offsetof(T, f5) == 40, "macdrv TABLE slot 5"); \
    _Static_assert(offsetof(T, f6) == 48, "macdrv TABLE slot 6"); \
    _Static_assert(offsetof(T, f7) == 56, "macdrv TABLE slot 7"); \
    _Static_assert(offsetof(T, f8) == 64, "macdrv TABLE slot 8"); \
    _Static_assert(offsetof(T, f9) == 72, "macdrv TABLE slot 9"); \
    _Static_assert(sizeof(T) == 80, "macdrv TABLE size")
#define RAVEN_MACDRV_DATA(T, f0, f1, f2, f3) \
    _Static_assert(offsetof(T, f0) == 0, "macdrv DATA slot 0"); \
    _Static_assert(offsetof(T, f1) == 8, "macdrv DATA slot 1"); \
    _Static_assert(offsetof(T, f2) == 16, "macdrv DATA slot 2"); \
    _Static_assert(offsetof(T, f3) == 24, "macdrv DATA slot 3"); \
    _Static_assert(sizeof(T) == 32, "macdrv DATA size")
#define RAVEN_MACDRV_LAYER(T, f0, f1, f2, f3, f4, f5, f6, f7) \
    _Static_assert(offsetof(T, f0) == 0, "macdrv LAYER slot 0"); \
    _Static_assert(offsetof(T, f1) == 8, "macdrv LAYER slot 1"); \
    _Static_assert(offsetof(T, f2) == 16, "macdrv LAYER slot 2"); \
    _Static_assert(offsetof(T, f3) == 24, "macdrv LAYER slot 3"); \
    _Static_assert(offsetof(T, f4) == 32, "macdrv LAYER slot 4"); \
    _Static_assert(offsetof(T, f5) == 33, "macdrv LAYER slot 5"); \
    _Static_assert(offsetof(T, f6) == 34, "macdrv LAYER slot 6"); \
    _Static_assert(offsetof(T, f7) == 36, "macdrv LAYER slot 7"); \
    _Static_assert(sizeof(T) == 40, "macdrv LAYER size")
