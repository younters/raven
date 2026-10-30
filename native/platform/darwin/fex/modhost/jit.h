// SPDX-License-Identifier: MIT
#pragma once
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <mutex>

// Standalone Tier 0 ownership; never claims Windows 4 KiB VM semantics.
class NativeJit {
  struct Region { void *base; size_t size; };
  std::vector<Region> regions;
  mutable std::mutex mutex;
public:
  NativeJit() = default;
  NativeJit(const NativeJit&) = delete;
  NativeJit& operator=(const NativeJit&) = delete;
  template<class F> void each_region(F visit) const { std::lock_guard<std::mutex> lock(mutex); for(auto r:regions) visit(r.base,r.size); }
  ~NativeJit() { for (auto r : regions) munmap(r.base, r.size); }
  void *allocate(size_t size) {
    if (!size || size > SIZE_MAX - 16383) throw std::runtime_error("bad JIT allocation size");
    size = (size + 16383) & ~size_t(16383);
    void *base = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (base == MAP_FAILED) throw std::runtime_error("MAP_JIT allocation failed");
    {std::lock_guard<std::mutex> lock(mutex);regions.push_back({base, size});}
    return base;
  }
  void *immutable(const void *code,size_t size) {
    auto rounded=(size+16383)&~size_t(16383);
    void *p=mmap(nullptr,rounded,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(p==MAP_FAILED)throw std::runtime_error("adapter allocation failed");
    memcpy(p,code,size);sys_icache_invalidate(p,size);
    if(mprotect(p,rounded,PROT_READ|PROT_EXEC)){munmap(p,rounded);throw std::runtime_error("adapter RX protection failed");}
    {std::lock_guard<std::mutex> lock(mutex);regions.push_back({p,rounded});}return p;
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
