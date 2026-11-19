#include "checked_memory.h"
#include <signal.h>
#include <string.h>
#include <stdlib.h>
static raven_unix_check_range memory_check = raven_unix_mapped_range;
void raven_unix_set_memory_check(raven_unix_check_range check)
{
    __atomic_store_n(&memory_check, check, __ATOMIC_RELEASE);
}
void *raven_unix_checked(const void *pointer, uint64_t count, uint64_t stride, uint32_t write)
{
    if ((stride && count > UINT64_MAX / stride) ||
        !__atomic_load_n(&memory_check, __ATOMIC_ACQUIRE)((uintptr_t)pointer, count * stride, write)) {
        // Explicit failed preflight, before a dereference. Dispatch owns recovery.
        raise(SIGSEGV);
        abort();
    }
    return (void *)pointer;
}
const char *raven_unix_string(const void *pointer)
{
    const char *text = pointer;
    uint64_t address = (uintptr_t)pointer;
    for (;;) {
        uint64_t count = 4096 - (address & 4095);
        const char *part = raven_unix_checked((void *)address, count, 1, 0);
        if (memchr(part, 0, count)) return text;
        if (address > UINT64_MAX - count) { raise(SIGSEGV); abort(); }
        address += count;
    }
}

void raven_unix_executable(const void *pointer)
{
    mach_vm_address_t start = (uintptr_t)pointer;
    mach_vm_size_t length = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(mach_task_self(), &start, &length,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object) mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS || start > (uintptr_t)pointer || !(info.protection & VM_PROT_EXECUTE))
        { raise(SIGSEGV); abort(); }
}

void *raven_unix_copy(void *destination, const void *source, size_t size)
{
    raven_unix_checked(destination, size, 1, 1);
    raven_unix_checked(source, size, 1, 0);
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    // Volatile vector accesses keep fault PCs in this helper without turning
    // large graphics uploads into a byte-at-a-time copy or a libc call.
    typedef unsigned char vector16 __attribute__((vector_size(16), aligned(1), may_alias));
    size_t i = 0;
    for (; size - i >= 64; i += 64) {
        *(volatile vector16 *)(out+i) = *(const volatile vector16 *)(in+i);
        *(volatile vector16 *)(out+i+16) = *(const volatile vector16 *)(in+i+16);
        *(volatile vector16 *)(out+i+32) = *(const volatile vector16 *)(in+i+32);
        *(volatile vector16 *)(out+i+48) = *(const volatile vector16 *)(in+i+48);
    }
    for (; size - i >= 16; i += 16)
        *(volatile vector16 *)(out+i) = *(const volatile vector16 *)(in+i);
    for (; i<size; ++i) out[i]=in[i];
    return destination;
}
