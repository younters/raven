// SPDX-License-Identifier: MIT
#pragma once
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

// Standalone Tier 0 ownership; never claims Windows 4 KiB VM semantics.
class NativeJit {
  struct Region { void *base; size_t size; };
  std::vector<Region> regions;
public:
  ~NativeJit() { for (auto r : regions) munmap(r.base, r.size); }
  void *allocate(size_t size) {
    if (!size || size > SIZE_MAX - 16383) throw std::runtime_error("bad JIT allocation size");
    size = (size + 16383) & ~size_t(16383);
    void *base = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (base == MAP_FAILED) throw std::runtime_error("MAP_JIT allocation failed");
    regions.push_back({base, size});
    return base;
  }
  inline static thread_local bool write_protected = true;
  static uint32_t protect(uint32_t mode) {
    if (mode == 2) return pthread_jit_write_protect_supported_np() ? 0 : UINT32_MAX;
    auto previous = write_protected;
    pthread_jit_write_protect_np(mode != 0);
    write_protected = mode != 0;
    return previous;
  }
  static void write(void *dest, const void *src, size_t size) {
    // Preserve the calling thread's write mode, including nested scopes.
    auto previous = protect(0);
    memcpy(dest, src, size);
    sys_icache_invalidate(dest, size);
    protect(previous);
  }
};
