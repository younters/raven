// SPDX-License-Identifier: MIT
#pragma once
#include "../unix_memory.h"
#include <map>
#include <mutex>
// Mirror logical 4K protections: Darwin's 16K union protection is insufficient
// for a unix call that straddles a guest PAGE_NOACCESS boundary.
class UnixRanges {
    struct Range { uint64_t end; int protection; };
    std::map<uint64_t, Range> ranges;
    std::mutex mutex;
    void split(uint64_t at) {
        auto it = ranges.upper_bound(at);
        if (it == ranges.begin()) return;
        --it;
        if (it->first < at && at < it->second.end) {
            const auto old = it->second;
            it->second.end = at;
            ranges.emplace(at, old);
        }
    }
public:
    void set(uint64_t address, uint64_t size, int protection) {
        if (!size || size > UINT64_MAX - address) return;
        std::lock_guard lock(mutex);
        split(address); split(address + size);
        ranges.erase(ranges.lower_bound(address), ranges.lower_bound(address + size));
        ranges.emplace(address, Range{address + size, protection});
    }
    int check(uint64_t address, uint64_t size, uint32_t write) {
        if (!size) return 1;
        if (!address || size > UINT64_MAX - address) return 0;
        std::lock_guard lock(mutex);
        auto it = ranges.upper_bound(address);
        if (it != ranges.begin()) --it;
        const int required = 1 | (write ? 2 : 0);
        uint64_t covered = address;
        for (; it != ranges.end() && it->first < address + size; ++it) {
            if (it->second.end <= address) continue;
            if ((it->second.protection & required) != required) return 0;
            if (it->first <= covered) covered = it->second.end;
            if (covered >= address + size) return 1;
        }
        return raven_unix_mapped_range(address, size, write);
    }
};
