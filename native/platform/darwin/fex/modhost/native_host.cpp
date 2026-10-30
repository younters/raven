// SPDX-License-Identifier: MIT
// M1c native host substrate. Refuses entry while any import/ABI gate is unmet.
#define RAVEN_HOST_LOADER
#include "pe_preflight.cpp"
#include "raven_host.h"
#include "abi.h"
#include "crt.h"
#include "sync.h"
#include "nt_strings.h"
#include "environment.h"
#include "vm.h"
#include "pe_view.h"
#include "nt_io.h"
#include "nt_system.h"
#include "format.h"
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
using RavenVM::allocate;
using RavenVM::release;
using RavenVM::protect;
using RavenVM::query;
using RavenVM::MemoryInfo;
constexpr int32_t invalid = int32_t(0xc000000d);
size_t page_size() { return RavenVM::page(); }
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
  ENTRY("NtAllocateVirtualMemory", allocate); ENTRY("NtAllocateVirtualMemoryEx", RavenVM::allocate_ex);
  ENTRY("NtFlushInstructionCache", RavenVM::flush); ENTRY("NtFreeVirtualMemory", release);
  ENTRY("NtProtectVirtualMemory", protect); ENTRY("NtQueryVirtualMemory", query);
  ENTRY("RtlAllocateHeap", heap_alloc); ENTRY("RtlFreeHeap", heap_free);
  ENTRY("RtlQueryPerformanceCounter", qpc);
  ENTRY("RtlInitializeCriticalSection", initialize_lock); ENTRY("RtlEnterCriticalSection", enter);
  ENTRY("RtlLeaveCriticalSection", leave); ENTRY("RtlTryEnterCriticalSection", try_enter);
  ENTRY("RtlDeleteCriticalSection", delete_lock);
#undef ENTRY
  if (auto *p = RavenCRT::resolve(name)) return p;
  if (auto *p = RavenSync::resolve(name)) return p;
  if (auto *p = RavenNT::resolve_strings(name)) return p;
  if (auto *p = RavenNT::resolve_system(name)) return p;
  return RavenFormat::resolve(name);
}
NativeJit import_jit;
void *resolve(const char *dll, const char *name) {
  unsigned stack = 0;
  auto *target = resolve_native(dll, name);
  if (!target && !strcmp(dll,"ntdll.dll")) target = RavenNT::resolve_io(name, stack);
  return target ? native_adapter(import_jit, target, stack) : nullptr;
}
std::unique_ptr<PEView> mapped_image;
std::unique_ptr<RavenNT::ProcessEnvironment> process_environment;
std::unique_ptr<RavenNT::ThreadEnvironment> thread_environment;
void inspect_image(Bytes image, size_t opt) {
  mapped_image = std::make_unique<PEView>(PEView{image,opt});
  RavenNT::active_image=mapped_image.get();
  process_environment=std::make_unique<RavenNT::ProcessEnvironment>();
  thread_environment=std::make_unique<RavenNT::ThreadEnvironment>(*process_environment);
  RavenNT::active_process=process_environment.get(); RavenNT::active_thread=thread_environment.get();
  RavenNT::import_lookup=resolve;
  process_environment->put(0x10,image.data);
  mapped_image->install_tls(*thread_environment);
  auto *entry=mapped_image->export_address("RavenProcessInit");
  if(!entry) throw std::runtime_error("missing RavenProcessInit native export");
  std::cout << "CHPE native RavenProcessInit RVA=" << std::hex << uintptr_t(entry)-uintptr_t(image.data) << std::dec
            << " TLS_bytes=" << thread_environment->static_tls.size() << " TEB/CPU-area prepared (not entered)\n";
}
void self_test() {
  assert(page_size() == 16384);
  char formatted[64]; uint64_t va[3]{42,uintptr_t("bird"),0x3ff4000000000000ull};
  assert(RavenFormat::output(formatted,64,"%d %s %.2f",va)==12 && !strcmp(formatted,"42 bird 1.25"));
  int scanned=0;char scanned_text[16]{};
  assert(RavenFormat::scan("42 bird","%d %15s",uintptr_t(&scanned),uintptr_t(scanned_text),nullptr,0)==2);
  assert(scanned==42 && !strcmp(scanned_text,"bird"));
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
  void *reservation=nullptr;size_t reserved_size=32768;
  assert(allocate(nullptr,&reservation,0,&reserved_size,0x2000,4)==0);
  auto *second=static_cast<char*>(reservation)+16384;void *commit=second;size_t committed=1;
  assert(allocate(nullptr,&commit,0,&committed,0x1000,4)==0 && committed==16384);
  memset(commit,0x6b,committed);
  size_t decommitted=committed;assert(release(nullptr,&commit,&decommitted,0x4000)==0);
  assert(allocate(nullptr,&commit,0,&committed,0x1000,4)==0);
  assert(*static_cast<char*>(commit)==0);
  size_t zero=0;assert(release(nullptr,&reservation,&zero,0x8000)==0);
  RavenVM::Extended ec{5,0x40};void *native_code=nullptr;size_t native_size=16;
  assert(RavenVM::allocate_ex(reinterpret_cast<void*>(-1),&native_code,&native_size,0x3000,0x40,&ec,1)==0);
  uint32_t ret_instruction=0xd65f03c0;NativeJit::write(native_code,&ret_instruction,4);
  reinterpret_cast<void(*)()>(native_code)();
  zero=0;assert(release(nullptr,&native_code,&zero,0x8000)==0);
  char temporary[]="/tmp/raven-modhost-io-XXXXXX";int fd=mkstemp(temporary);assert(fd>=0);close(fd);
  auto path=RavenNT::decode(temporary,strlen(temporary));RavenNT::String name{};RavenNT::init_unicode(&name,path.c_str());
  RavenNT::ObjectAttributes object{48,0,nullptr,&name,0,0,nullptr,nullptr};
  RavenNT::IoStatus io{};void *file=nullptr;
  assert(RavenNT::create_file(&file,0xc0000000,&object,&io,nullptr,0,7,1,0x20,0,0)==0);
  int64_t position=0;
  assert(RavenNT::write_file(file,nullptr,nullptr,nullptr,&io,"bird",4,&position,0)==0 && io.information==4);
  char readback[4]{};assert(RavenNT::read_file(file,nullptr,nullptr,nullptr,&io,readback,4,&position,0)==0 && !memcmp(readback,"bird",4));
  void *section=nullptr;assert(RavenNT::create_section(&section,0,nullptr,nullptr,2,0x8000000,file)==0);
  void *view=nullptr;size_t view_size=0;
  assert(RavenNT::map_section(section,reinterpret_cast<void*>(-1),&view,0,0,nullptr,&view_size,1,0,2)==0 && view_size==4 && !memcmp(view,"bird",4));
  assert(RavenNT::unmap_section(reinterpret_cast<void*>(-1),view)==0);
  assert(RavenNT::close_handle(section)==0 && RavenNT::close_handle(file)==0);unlink(temporary);
  RavenNT::ProcessEnvironment process;
  RavenNT::ThreadEnvironment thread(process);
  uint32_t initial_tls = 0x12345678;
  thread.install_tls(&initial_tls,4,12);
  assert(*static_cast<uint32_t*>(thread.tls_vector[0]) == initial_tls);
  assert(thread.static_tls[15]==0 && thread.cpu.context==thread.context.data());
  assert(*reinterpret_cast<void**>(thread.teb.data()+0x1788)==&thread.cpu);
  auto utf16 = RavenNT::decode("A\xf0\x9f\x90\xa6", 5);
  assert(utf16 == u"A\U0001f426" && RavenNT::encode(utf16.data(), utf16.size()) == "A\xf0\x9f\x90\xa6");
  uint32_t count = 0; char16_t tiny[2] {};
  assert(RavenNT::to_unicode(tiny, 4, &count, "A\xf0\x9f\x90\xa6", 5) == int32_t(0x80000005) && count == 2);
  uint32_t bits[2] {1,0}; RavenNT::Bitmap bitmap {64,0,bits};
  assert(RavenNT::find_clear(&bitmap,2,1)==1 && bits[0]==7);
  RavenNT::clear_bits(&bitmap,1,2); assert(bits[0]==1);
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
  return map_module(argc, argv, resolve, inspect_image);
}
