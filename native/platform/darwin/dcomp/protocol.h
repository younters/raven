// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// A private, bounded SetPropW message from the PE DirectComposition component.
// The host copies and validates the complete transaction before publishing it.
#define RAVEN_DCOMP_COMMIT_MAGIC 0x52444331U
#define RAVEN_DCOMP_MAX_VISUALS 128
struct raven_dcomp_item {
	uint64_t stream, target;
	float x, y, sx, sy;
	float clip[4];
	float rounded[4], radius[8];
	int32_t order;
	uint32_t linear;
};
struct raven_dcomp_commit {
	uint32_t magic, count;
	struct raven_dcomp_item items[RAVEN_DCOMP_MAX_VISUALS];
};

#ifdef __cplusplus
static_assert(sizeof(raven_dcomp_item) == 104, "composition item ABI");
#endif
