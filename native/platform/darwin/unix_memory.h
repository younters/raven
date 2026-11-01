// SPDX-License-Identifier: MIT
// Shared by the controller, Rosetta agent and provisioned unix libraries.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

typedef int (*raven_unix_check_range)(uint64_t, uint64_t, uint32_t);
#define RAVEN_UNIX_CONTRACT_VERSION 1u
struct raven_unix_contract {
    uint32_t version, count;
    const uint32_t *argument_sizes;
    int (*validate)(uint32_t, void *, raven_unix_check_range);
    void (*set_memory_check)(raven_unix_check_range);
};

#ifdef __cplusplus
static_assert(sizeof(raven_unix_contract) == 32);
static_assert(offsetof(raven_unix_contract, argument_sizes) == 8);
static_assert(offsetof(raven_unix_contract, validate) == 16);
static_assert(offsetof(raven_unix_contract, set_memory_check) == 24);
#else
_Static_assert(sizeof(struct raven_unix_contract) == 32, "unix contract ABI");
_Static_assert(offsetof(struct raven_unix_contract, argument_sizes) == 8, "unix sizes slot");
_Static_assert(offsetof(struct raven_unix_contract, validate) == 16, "unix validator slot");
_Static_assert(offsetof(struct raven_unix_contract, set_memory_check) == 24, "unix check slot");
#endif

// Inspect the complete range, including every region boundary. No speculative
// load/store or destructive write probe is used. The dispatch fault guard covers
// concurrent unmap/protect after this check.
static inline int raven_unix_mapped_range(uint64_t address, uint64_t size, uint32_t write)
{
    if (!size) return 1;
    if (!address || size > UINT64_MAX - address) return 0;
    uint64_t end = address + size;
    while (address < end) {
        mach_vm_address_t start = address;
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t status = mach_vm_region(mach_task_self(), &start, &length,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        int required = VM_PROT_READ | (write ? VM_PROT_WRITE : 0);
        if (status != KERN_SUCCESS || start > address || !length ||
            (info.protection & required) != required || length > UINT64_MAX - start) return 0;
        address = start + length;
    }
    return 1;
}
