#include "slot_state.h"
#include <stdio.h>
#include <stdlib.h>

static void check(bool ok, const char *message)
{
	if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}

int main(void)
{
	// The same helpers execute local, foreign-release, composition-release and
	// import/forward completion transitions in the host presentation pump.
	struct raven_surface_frame frame = {.state = RAVEN_SURFACE_DISPLAYED, .sequence = 12};
	raven_surface_release(&frame, 11);
	check(frame.state == RAVEN_SURFACE_DISPLAYED, "stale release preserves newer lease");
	raven_surface_release(&frame, 12);
	check(frame.state == RAVEN_SURFACE_AVAILABLE, "current release acknowledges lease");
	const uint32_t results[] = {RAVEN_SURFACE_AVAILABLE, RAVEN_SURFACE_DISPLAYED};
	for (unsigned i = 0; i < 2; ++i) {
		frame.state = RAVEN_SURFACE_IMPORTING;
		check(!raven_surface_transition(&frame, 11, RAVEN_SURFACE_IMPORTING, results[i]) &&
			frame.state == RAVEN_SURFACE_IMPORTING, "stale import/forward sequence rejected");
		frame.state = RAVEN_SURFACE_RETIRE;
		check(!raven_surface_transition(&frame, 12, RAVEN_SURFACE_IMPORTING, results[i]) &&
			frame.state == RAVEN_SURFACE_RETIRE, "import failure/success cannot clobber RETIRE");
		frame.state = RAVEN_SURFACE_IMPORTING;
		check(raven_surface_transition(&frame, 12, RAVEN_SURFACE_IMPORTING, results[i]) &&
			frame.state == results[i], "matching import/forward completion accepted");
	}
	puts("host release and import/forward sequence and retirement transitions PASS");
	return 0;
}
