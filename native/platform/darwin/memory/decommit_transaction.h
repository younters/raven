#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <sys/mman.h>
namespace raven {
struct DecommitRun { uint64_t address, size; int protection; };
// Metadata is committed by the caller only after this operation succeeds.
// Discard can fail after changing permissions on any run, including the last.
template<class Protect, class Discard>
void decommit_transaction(std::span<const DecommitRun> runs, uint64_t base, uint64_t size,
    Protect protect, Discard discard)
{
    try {
        for (const auto &run : runs) {
            protect(run.address,run.size,PROT_READ|PROT_WRITE);
            discard(run.address,run.size);
        }
        protect(base,size,PROT_NONE);
    } catch (...) {
        for (const auto &run : runs) protect(run.address,run.size,run.protection);
        throw;
    }
}
}
