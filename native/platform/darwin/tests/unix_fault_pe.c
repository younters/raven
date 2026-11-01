#include <stdint.h>
struct request { uint64_t gate, handle, args; uint32_t code; };
uint32_t mainCRTStartup(struct request *p) {
    typedef uint32_t (__attribute__((stdcall)) *gate_t)(uint64_t, uint32_t, void *);
    return ((gate_t)(uintptr_t)p->gate)(p->handle, p->code, (void *)(uintptr_t)p->args);
}
