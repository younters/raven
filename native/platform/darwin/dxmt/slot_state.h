#pragma once
#include "../guest/protocol.h"
#include <stdbool.h>

// Host callers run on the presentation pump. A stale acknowledgement must not
// alter a newer lease, and completion must never overwrite a guest RETIRE.
static inline bool raven_surface_transition(struct raven_surface_frame *frame, uint64_t sequence,
	uint32_t expected, uint32_t next)
{
	if (frame->sequence != sequence) return false;
	return __atomic_compare_exchange_n(&frame->state, &expected, next, 0,
		__ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static inline void raven_surface_release(struct raven_surface_frame *frame, uint64_t sequence)
{
	(void)raven_surface_transition(frame, sequence, RAVEN_SURFACE_DISPLAYED, RAVEN_SURFACE_AVAILABLE);
}
