#include "../guest/protocol.h"
#include <stdlib.h>

#ifndef REAL_ADAPTER
#if ADAPTER_VERSION
uint32_t raven_unixlib_protocol_version(void) { return ADAPTER_VERSION; }
#endif
void raven_unixlib_init(struct raven_control *control)
{
	// Rejected adapters must never receive the shared control pointer.
	if (ADAPTER_VERSION != RAVEN_GUEST_VERSION || control->version != RAVEN_GUEST_VERSION) abort();
}
#endif
static int32_t unused(void *args) { (void)args; return 0; }
void *__wine_unix_call_funcs[132] = {unused};
