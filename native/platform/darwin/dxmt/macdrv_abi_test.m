#include "../unix_guard.h"
#include "../guest/protocol.h"
#include <assert.h>
struct raven_control *raven_dxmt_control(void) { return NULL; }
#include "presentation.m"
static void fault(int signal) { raven_unix_recover(signal); _Exit(2); }
static int validate(uint32_t code, void *args, raven_unix_check_range check) { (void)code; (void)args; (void)check; return 1; }
static void set_check(raven_unix_check_range check) { (void)check; }
static int32_t invoke(void *block) { macdrv_functions.main((dispatch_block_t)block); return 0; }
int main(void) {
    signal(SIGSEGV, fault); signal(SIGBUS, fault);
    static const uint32_t sizes[] = {0};
    const struct raven_unix_contract contract = {1, 1, sizes, validate, set_check};
    const int32_t av = (int32_t)0xc0000005;
    assert(raven_unix_invoke(invoke, (void *)1, 0, &contract, raven_unix_mapped_range) == av);
    struct literal { void *isa; int flags, reserved; void (*invoke)(void *); const uint64_t *descriptor; };
    uint64_t descriptor[] = {0, sizeof(struct literal)};
    struct literal bad = {0, 0, 0, (void *)1, descriptor};
    assert(raven_unix_invoke(invoke, &bad, 0, &contract, raven_unix_mapped_range) == av);
    bad.descriptor = (void *)1;
    assert(raven_unix_invoke(invoke, &bad, 0, &contract, raven_unix_mapped_range) == av);
    __block int calls = 0;
    dispatch_block_t good = ^{ ++calls; };
    assert(raven_unix_invoke(invoke, (void *)good, 0, &contract, raven_unix_mapped_range) == 0);
    assert(calls == 1);
    macdrv_functions.main(NULL);
    puts("macdrv table/data/layer ABI and block validation passed");
}
