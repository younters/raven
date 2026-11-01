#include "checked_memory.h"
#include <signal.h>
#include <string.h>
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
        return NULL;
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
        if (address > UINT64_MAX - count) { raise(SIGSEGV); return NULL; }
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
        raise(SIGSEGV);
}

void *raven_unix_copy(void *destination, const void *source, size_t size)
{
    raven_unix_checked(destination, size, 1, 1);
    raven_unix_checked(source, size, 1, 0);
    return memcpy(destination, source, size);
}
