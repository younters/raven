// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// Opt-in inclusive timings. Each process reports separately at normal exit.
// Counters are atomic because parallel guest workers can use these paths.
namespace fex_perf {
inline bool enabled() {
    static const bool value = std::getenv("RAVEN_FEX_PERF") != nullptr;
    return value;
}
struct Counter {
    const char *name;
    std::atomic<uint64_t> calls{}, ns{};
    ~Counter() {
        if (enabled()) std::fprintf(stderr, "FEX_PERF pid=%d name=%s calls=%llu ns=%llu\n",
            getpid(), name, (unsigned long long)calls.load(), (unsigned long long)ns.load());
    }
};
struct Timer {
    Counter &counter;
    bool active = enabled();
    std::chrono::steady_clock::time_point start;
    explicit Timer(Counter &c) : counter(c) {
        if (active) start = std::chrono::steady_clock::now();
    }
    ~Timer() {
        if (!active) return;
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        counter.calls.fetch_add(1, std::memory_order_relaxed);
        counter.ns.fetch_add(elapsed, std::memory_order_relaxed);
    }
};
}
