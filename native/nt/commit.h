#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace raven {

class ApiCall;
class NativeProcess;
class NativeMemory;
struct KernelContext;

uint32_t commit_system_information(NativeProcess &, NativeMemory &, std::span<const uint64_t>);
void commit_global_status_ex(ApiCall &, NativeMemory &);
void commit_global_status(ApiCall &, NativeMemory &);
void commit_performance_info(ApiCall &, KernelContext &);
void commit_process_memory_info(ApiCall &, KernelContext &);
struct HostMemory { uint64_t total, available, swap_used; };
HostMemory host_memory();
size_t system_commit_limit();
uint64_t system_commit_charge(uint64_t own);
uint64_t system_commit_peak(uint64_t own);
}
