#include "../unix_memory.h"
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

static const uint32_t sizes[132] = {0};
static int validate(uint32_t code, void *args, raven_unix_check_range check) { (void)args; (void)check; return code < 132; }
static void set_check(raven_unix_check_range check) { (void)check; }
const struct raven_unix_contract *raven_unix_get_contract(void) {
    static const struct raven_unix_contract c = {1, sizeof(__wine_unix_call_funcs)/sizeof(__wine_unix_call_funcs[0]), sizes, validate, set_check};
    return &c;
}
