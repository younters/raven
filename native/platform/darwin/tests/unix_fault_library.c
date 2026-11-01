#include "../unix_memory.h"
#include "../guest/protocol.h"
#include <stdint.h>

struct args { uint64_t pointer, length; uint32_t mode, observed; };
static uint64_t base;
static uint64_t host(uint64_t address) { return base && address <= UINT32_MAX ? base + address : address; }
static int32_t call(void *opaque) {
    struct args *p = opaque;
    // code 1 deliberately bypasses nested preflight, exercising the backstop.
    if (p->mode) p->observed = *(volatile uint8_t *)(uintptr_t)host(p->pointer + p->length - 1);
    else p->observed = 42;
    return 0;
}
#ifndef TABLE_COUNT
#define TABLE_COUNT 22
#endif
void *__wine_unix_call_funcs[TABLE_COUNT] = {call, call};
void *__wine_unix_call_wow64_funcs[TABLE_COUNT] = {call, call};
static const uint32_t sizes[TABLE_COUNT] = {sizeof(struct args), sizeof(struct args)};
static int validate(uint32_t code, void *opaque, raven_unix_check_range check) {
    struct args *p = opaque;
    if (code == 1 || !p->mode) return 1;
    if (base && p->pointer <= UINT32_MAX && p->length > (1ull << 32) - p->pointer) return 0;
    return check(host(p->pointer), p->length, 0);
}
static void set_check(raven_unix_check_range check) { (void)check; }
const struct raven_unix_contract *raven_unix_get_contract(void) {
    static const struct raven_unix_contract c = {1, sizeof(__wine_unix_call_funcs)/sizeof(__wine_unix_call_funcs[0]), sizes, validate, set_check};
    return &c;
}
void raven_mtld3d_set_guest_base(uint64_t value) { base = value; }
int raven_mtld3d_bind(const void *callbacks) { return callbacks ? 0 : -1; }
uint32_t raven_mtld3d_protocol_version(void) { return RAVEN_GUEST_VERSION; }
void raven_mtld3d_init(void *control) { (void)control; }
void raven_mtld3d_layer_create(void) {}
void raven_mtld3d_layer_release(void) {}
void raven_mtld3d_present(void) {}
