// SPDX-License-Identifier: MIT
// M1c native host substrate. Refuses entry while any import/ABI gate is unmet.
#define RAVEN_HOST_LOADER
#include "pe_preflight.cpp"
#include "raven_host.h"
#include "abi.h"
#include "crt.h"
#include "sync.h"
#include <thread>
#include <atomic>
#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cassert>

namespace {
constexpr int32_t invalid = int32_t(0xc000000d), no_memory = int32_t(0xc0000017);
size_t page_size() { return size_t(getpagesize()); }
size_t host_round_size(size_t n) {
  if (!n || n > SIZE_MAX - page_size() + 1) return 0;
  return (n + page_size() - 1) & ~(page_size() - 1);
}
int protection(uint32_t p) {
  switch (p & 255) {
  case 1: return PROT_NONE;
  case 2: return PROT_READ;
  case 4: return PROT_READ | PROT_WRITE;
  case 0x10: return PROT_EXEC;
  case 0x20: return PROT_READ | PROT_EXEC;
  case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;
  default: return -1;
  }
}
struct Region { size_t size; uint32_t protect, state; };
std::mutex vm_mutex;
std::map<uintptr_t, Region> regions;
int32_t allocate(void *, void **base, uintptr_t zero_bits, size_t *size, uint32_t type, uint32_t prot) {
  if (!base || !size || zero_bits || *base || (type != 0x3000 && type != 0x2000)) return invalid;
  auto n = host_round_size(*size); auto p = protection(prot);
  if (!n || p < 0 || (prot & ~255u)) return invalid;
  // Executable native JIT requires the explicit MAP_JIT hook, never RWX mmap.
  if (p & PROT_EXEC) return int32_t(0xc00000bb);
  void *v = mmap(nullptr, n, type == 0x2000 ? PROT_NONE : p, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (v == MAP_FAILED) return no_memory;
  { std::lock_guard<std::mutex> lock(vm_mutex); regions.emplace(uintptr_t(v), Region{n, type == 0x2000 ? 1u : prot, type == 0x2000 ? 0x2000u : 0x1000u}); }
  *base = v; *size = n; return 0;
}
int32_t release(void *, void **base, size_t *size, uint32_t type) {
  if (!base || !size || type != 0x8000 || *size) return invalid;
  std::lock_guard<std::mutex> lock(vm_mutex);
  auto it = regions.find(uintptr_t(*base));
  if (it == regions.end() || munmap(*base, it->second.size)) return invalid;
  regions.erase(it); *base = nullptr; return 0;
}
int32_t protect(void *, void **base, size_t *size, uint32_t prot, uint32_t *old) {
  if (!base || !size || !old || (uintptr_t(*base) % page_size()) || protection(prot) < 0 || (prot & ~255u)) return invalid;
  std::lock_guard<std::mutex> lock(vm_mutex);
  auto it = regions.find(uintptr_t(*base)); auto n = host_round_size(*size);
  // Deliberately require a whole owned region: mixed/subpage protection isn't modeled.
  if (it == regions.end() || n != it->second.size || (protection(prot) & PROT_EXEC)) return invalid;
  if (mprotect(*base, n, protection(prot))) return invalid;
  *old = it->second.protect; it->second.protect = prot; *size = n; return 0;
}
struct MemoryInfo {
  void *base, *allocation_base;
  uint32_t allocation_protect, pad;
  uint64_t region_size;
  uint32_t state, protect, type, pad2;
};
static_assert(sizeof(MemoryInfo) == 48);
int32_t query(void *, void *address, uint32_t klass, void *output, size_t length, size_t *written) {
  if (klass || !output || length < sizeof(MemoryInfo)) return invalid;
  std::lock_guard<std::mutex> lock(vm_mutex);
  auto it = regions.upper_bound(uintptr_t(address));
  if (it == regions.begin()) return invalid;
  --it;
  if (uintptr_t(address) - it->first >= it->second.size) return invalid;
  auto &r = it->second;
  *static_cast<MemoryInfo*>(output) = {reinterpret_cast<void*>(it->first), reinterpret_cast<void*>(it->first), r.protect, 0,
                                    r.size, r.state, r.protect, 0x20000, 0};
  if (written) *written = sizeof(MemoryInfo);
  return 0;
}
void *heap_alloc(void *, uint32_t flags, size_t size) { if (flags & ~8u) return nullptr; return (flags & 8) ? calloc(1, size ? size : 1) : malloc(size ? size : 1); }
uint8_t heap_free(void *, uint32_t, void *p) { free(p); return 1; }
int32_t qpc(int64_t *counter, int64_t *frequency) {
  if (!counter) return invalid;
  *counter = int64_t(mach_absolute_time());
  if (frequency) {
    mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
    *frequency = int64_t(1000000000ull * timebase.denom / timebase.numer);
  }
  return 1;
}
std::mutex locks_mutex;
std::map<void *, std::unique_ptr<std::recursive_mutex>> locks;
int32_t initialize_lock(void *key) {
  if (!key) return invalid;
  std::lock_guard<std::mutex> lock(locks_mutex);
  if (locks.count(key)) return invalid;
  locks[key] = std::make_unique<std::recursive_mutex>(); return 0;
}
std::recursive_mutex *get_lock(void *key) {
  std::lock_guard<std::mutex> lock(locks_mutex);
  auto it = locks.find(key);
  if (it == locks.end()) { std::cerr << "uninitialized critical section\n"; std::abort(); }
  return it->second.get();
}
int32_t enter(void *key) { get_lock(key)->lock(); return 0; }
int32_t leave(void *key) { get_lock(key)->unlock(); return 0; }
uint8_t try_enter(void *key) { return get_lock(key)->try_lock(); }
int32_t delete_lock(void *key) {
  std::lock_guard<std::mutex> lock(locks_mutex);
  return locks.erase(key) ? 0 : invalid;
}
void *resolve_native(const char *dll, const char *name) {
  if (std::string(dll) != "ntdll.dll") return nullptr;
#define ENTRY(n, f) if (std::string(name) == n) return reinterpret_cast<void*>(&f)
  ENTRY("NtAllocateVirtualMemory", allocate); ENTRY("NtFreeVirtualMemory", release);
  ENTRY("NtProtectVirtualMemory", protect); ENTRY("NtQueryVirtualMemory", query);
  ENTRY("RtlAllocateHeap", heap_alloc); ENTRY("RtlFreeHeap", heap_free);
  ENTRY("RtlQueryPerformanceCounter", qpc);
  ENTRY("RtlInitializeCriticalSection", initialize_lock); ENTRY("RtlEnterCriticalSection", enter);
  ENTRY("RtlLeaveCriticalSection", leave); ENTRY("RtlTryEnterCriticalSection", try_enter);
  ENTRY("RtlDeleteCriticalSection", delete_lock);
#undef ENTRY
  if (auto *p = RavenCRT::resolve(name)) return p;
  return RavenSync::resolve(name);
}
NativeJit import_jit;
void *resolve(const char *dll, const char *name) {
  auto *target = resolve_native(dll, name);
  return target ? native_adapter(import_jit, target) : nullptr;
}
void self_test() {
  assert(page_size() == 16384);
  uint64_t srw = 0, condition = 0;
  RavenSync::init_rw(&srw); RavenSync::init_cv(&condition);
  bool ready = false;
  std::atomic<bool> started {false};
  std::thread worker([&] {
    RavenSync::acquire_ex(&srw); started = true;
    while (!ready) {
      int64_t timeout = -10000000;
      auto result = RavenSync::sleep_cv(&condition, &srw, &timeout, 0);
      assert(result == 0 || result == 0x102);
    }
    RavenSync::release_ex(&srw);
  });
  while (!started) std::this_thread::yield();
  RavenSync::acquire_ex(&srw); ready = true; RavenSync::wake_cv(&condition); RavenSync::release_ex(&srw);
  worker.join();
  uint64_t address_value = 1, compare_value = 1;
  int64_t immediate = 0;
  assert(RavenSync::wait_address(&address_value, &compare_value, 8, &immediate) == 0x102);
  compare_value = 2;
  assert(RavenSync::wait_address(&address_value, &compare_value, 8, nullptr) == 0);
  char16_t text[] = u"4294967295!"; char16_t *end = nullptr;
  assert(RavenCRT::wide_ulong(text, &end, 10) == UINT32_MAX && *end == '!');
  errno = 0;
  assert(RavenCRT::wide_long(text, &end, 10) == INT32_MAX && errno == ERANGE);
  assert(RavenCRT::length(u"\U0001f426") == 2);
  assert(RavenCRT::compare(u"bird", u"birds") < 0);
  char *last = nullptr;
  assert(RavenCRT::ansi_ulong("-1!", &last, 10) == UINT32_MAX && *last == '!');
  uint64_t args[8] {reinterpret_cast<uintptr_t>(text)};
  uint64_t teb[1024] {};
  auto *length_adapter = resolve("ntdll.dll", "wcslen");
  assert(length_adapter && raven_ec_call(length_adapter, args, teb) == 11);
  void *p = nullptr; size_t n = 1;
  assert(allocate(nullptr, &p, 0, &n, 0x3000, 4) == 0 && n == page_size());
  memset(p, 0x5a, n);
  MemoryInfo info {}; size_t written = 0;
  assert(query(nullptr, p, 0, &info, sizeof(info), &written) == 0 && written == 48 && info.region_size == n);
  uint32_t old = 0;
  assert(protect(nullptr, &p, &n, 2, &old) == 0 && old == 4);
  assert(static_cast<uint8_t*>(p)[n - 1] == 0x5a);
  assert(protect(nullptr, &p, &n, 4, &old) == 0 && old == 2);
  n = 0; assert(release(nullptr, &p, &n, 0x8000) == 0 && p == nullptr);
  auto *heap = static_cast<uint8_t*>(heap_alloc(nullptr, 8, 64));
  assert(heap && heap[63] == 0); assert(heap_free(nullptr, 0, heap));
  int key = 0; assert(initialize_lock(&key) == 0); assert(enter(&key) == 0);
  assert(try_enter(&key)); assert(leave(&key) == 0); assert(leave(&key) == 0); assert(delete_lock(&key) == 0);
  int64_t a, b, f; assert(qpc(&a, &f) == 1 && f > 0); assert(qpc(&b, nullptr) == 1 && b >= a);
  std::cout << "PASS: native VM, heap, CRT, SRW/condition/address waits, QPC substrate (not PE execution)\n";
}
}
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--self-test") { self_test(); return 0; }
  return map_module(argc, argv, resolve);
}
