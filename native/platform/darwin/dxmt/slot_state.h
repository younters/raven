#pragma once
#include "../guest/protocol.h"

// Host callers run on the presentation pump. A stale release must not retire a
// newer presentation which has since reused this metadata slot.
static inline void raven_surface_release(struct raven_surface_frame *frame, uint64_t sequence)
{
	if (frame->sequence != sequence) return;
	uint32_t expected = RAVEN_SURFACE_DISPLAYED;
	__atomic_compare_exchange_n(&frame->state, &expected, RAVEN_SURFACE_AVAILABLE, 0,
		__ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
