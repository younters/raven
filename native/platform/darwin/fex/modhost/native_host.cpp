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
#include "nt_registry.h"
#include "nt_threads.h"
#include "context.h"
#include "unwind.h"
#include "fault.h"
#include "bench.h"
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
#include <iomanip>

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
uint8_t rtl_qpc(int64_t *counter) { if(!counter)return 0;*counter=mach_absolute_time();return 1; }
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
  ENTRY("RtlQueryPerformanceCounter", rtl_qpc);
  ENTRY("RtlInitializeCriticalSection", initialize_lock); ENTRY("RtlEnterCriticalSection", enter);
  ENTRY("RtlLeaveCriticalSection", leave); ENTRY("RtlTryEnterCriticalSection", try_enter);
  ENTRY("RtlDeleteCriticalSection", delete_lock);
#undef ENTRY
  if (auto *p = RavenCRT::resolve(name)) return p;
  if (auto *p = RavenSync::resolve(name)) return p;
  if (auto *p = RavenNT::resolve_strings(name)) return p;
  if (auto *p = RavenNT::resolve_system(name)) return p;
  if (auto *p = RavenFormat::resolve(name)) return p;
  if (auto *p = RavenNT::resolve_registry(name)) return p;
  if (auto *p = RavenNT::resolve_context(name)) return p;
  return RavenNT::resolve_unwind(name);
}
NativeJit import_jit;
void *resolve(const char *dll, const char *name) {
  if (!strcmp(dll,"ntdll.dll") && !strcmp(name,"RtlCaptureContext")) return reinterpret_cast<void*>(&RavenNT::raven_capture_ec_context);
  if (!strcmp(dll,"ntdll.dll") && !strcmp(name,"RtlRestoreContext")) return reinterpret_cast<void*>(&RavenNT::raven_restore_ec_context);
  unsigned stack = 0;
  auto *target = !strcmp(dll,"ntdll.dll") ? RavenNT::resolve_threads(name,stack) : nullptr;
  if (!target) target = resolve_native(dll, name);
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
  RavenNT::import_lookup=resolve; RavenNT::pseudo_handle=RavenNT::current_handle;
  process_environment->put(0x10,image.data);
  mapped_image->install_tls(*thread_environment);
  auto *entry=mapped_image->export_address("RavenProcessInit");
  if(!entry) throw std::runtime_error("missing RavenProcessInit native export");
  std::cout << "CHPE native RavenProcessInit RVA=" << std::hex << uintptr_t(entry)-uintptr_t(image.data) << std::dec
            << " TLS_bytes=" << thread_environment->static_tls.size() << " TEB/CPU-area prepared (not entered)\n";
}
extern "C" void raven_check_ec_call();
std::array<uint8_t,16384> user_shared_data {};
NativeJit callback_jit;
sigjmp_buf guest_return;
sigjmp_buf *active_return=&guest_return;
bool exercise_nested=false,nested_done=false;uint64_t nested_result=0,nested_code=0;
uint64_t expected_resumed=4884;
unsigned nested_calls=0;
uint64_t timed_calls=0,expected_timed_calls=100000;
uint64_t integer_expected=5292597211099207303ull;
uint64_t float_expected_bits=0;
uint64_t invoke_nested(void *packet);
bool guest_active=false,guest_ok=false;
unsigned guest_calls=0;
uint64_t expected_guest_end=0;
uint64_t expected_fault_rip=0,expected_fault_address=0;unsigned faults_repaired=0;
uint64_t guest_dispatch(uint64_t number,void *packet) {
  auto *context=static_cast<uint8_t*>(packet);
  auto reg=[&](size_t offset){uint64_t v;memcpy(&v,context+offset,8);return v;};
  if(!guest_active)std::abort();
  if(number==0x100){
    ++guest_calls;
    if(reg(0xc8)!=7 || reg(0x88)!=11 || reg(0xb8)!=13 || reg(0xc0)!=17){guest_ok=false;siglongjmp(*active_return,1);}
    auto result=reg(0xc8)+3*reg(0x88)+5*reg(0xb8)+7*reg(0xc0);
    if(exercise_nested)result+=invoke_nested(packet);
    return result;
  }
  if(number==0x104 || number==0x105 || number==0x107){
    if(number==0x104)guest_ok=reg(0xc8)==integer_expected;
    else if(number==0x105)guest_ok=reg(0x1a0)==float_expected_bits;
    else guest_ok=reg(0xc8)==expected_timed_calls && timed_calls==expected_timed_calls;
    guest_ok=guest_ok && reg(0xf8)==expected_guest_end;siglongjmp(*active_return,1);
  }
  if(number==0x106){++timed_calls;return 1;}
  if(number==0x103){nested_result=reg(0xc8);nested_done=true;++nested_calls;siglongjmp(*active_return,1);}
  if(number==0x102){guest_ok=faults_repaired==1 && reg(0xc8)==4665 && reg(0xf8)==expected_guest_end;siglongjmp(*active_return,1);}
  if(number==0x101){guest_ok=guest_calls==1 && reg(0xc8)==expected_resumed && reg(0xf8)==expected_guest_end;siglongjmp(*active_return,1);}
  guest_ok=false;siglongjmp(*active_return,1);
}
int32_t unix_call(uint64_t,uint32_t,void *) { return RavenNT::not_supported; }
int32_t exception_callback(void *opaque,void *packet,void *) {
  auto *record=static_cast<RavenFault::Record*>(opaque);auto *context=static_cast<uint8_t*>(packet);
  uint64_t rip=0;memcpy(&rip,context+0xf8,8);
  if(record->code!=0xc0000005 || record->count!=2 || record->information[0]!=0 || record->information[1]!=expected_fault_address || rip!=expected_fault_rip || record->address!=reinterpret_cast<void*>(rip))return RavenNT::not_supported;
  ++faults_repaired;rip+=3;uint64_t value=0x1234;memcpy(context+0xf8,&rip,8);memcpy(context+0x78,&value,8);return 0;
}
void *cpu_area(void *) { return &thread_environment->cpu; }
void host_log(const char *message) { std::cerr << "FEX: " << message << std::flush; }
uint32_t jit_protect(uint32_t mode) { auto previous=NativeJit::protect(mode); if(getenv("RAVEN_MODHOST_TRACE"))fprintf(stderr,"jit mode=%u previous=%u\n",mode,previous); return previous; }
void *native_jit(uint64_t size) { auto *p=callback_jit.allocate(size);process_environment->mark_ec(uintptr_t(p),size);return p; }
void memory_event(uint64_t address,uint64_t size,uint64_t kind) {
  // FEX explicitly identifies native cache reservations that may overcommit.
  // Darwin mprotect commits access without physically populating demand-zero pages.
  if(kind==1){void *base=reinterpret_cast<void*>(address);size_t n=size;
    if(RavenVM::allocate(reinterpret_cast<void*>(-1),&base,0,&n,0x1000,4))std::abort();
  }else if(kind!=2)std::abort();
}
uint64_t invoke_nested(void *packet) {
  struct Frame {sigjmp_buf completion;alignas(16) uint8_t outer_context[0x4d0];uint64_t stack_base,stack_limit;};
  auto frame=std::make_unique<Frame>();memcpy(frame->outer_context,packet,sizeof(frame->outer_context));
  auto &cpu=thread_environment->cpu;frame->stack_base=cpu.stack_base;frame->stack_limit=cpu.stack_limit;
  void *emulator_stack=nullptr,*stack=nullptr;size_t emulator_size=262144,stack_size=262144;
  if(allocate(nullptr,&emulator_stack,0,&emulator_size,0x3000,4) || allocate(nullptr,&stack,0,&stack_size,0x3000,4))std::abort();
  cpu.stack_limit=uintptr_t(emulator_stack);cpu.stack_base=cpu.stack_limit+emulator_size;
  auto *context=static_cast<uint8_t*>(packet);auto sp=uintptr_t(stack)+stack_size-128;uint64_t argument=41;
  memcpy(context+0xf8,&nested_code,8);memcpy(context+0x98,&sp,8);memcpy(context+0x80,&argument,8);
  auto *previous=active_return;active_return=&frame->completion;nested_done=false;
  uint64_t args[8]{};
  if(!sigsetjmp(frame->completion,1))raven_ec_call(mapped_image->export_address("BeginSimulation"),args,thread_environment->teb.data());
  active_return=previous;
  cpu.stack_base=frame->stack_base;cpu.stack_limit=frame->stack_limit;memcpy(packet,frame->outer_context,sizeof(frame->outer_context));
  size_t zero=0;release(nullptr,&emulator_stack,&zero,0x8000);zero=0;release(nullptr,&stack,&zero,0x8000);
  if(!nested_done || nested_result!=124)std::abort();return nested_result;
}
void execute_image(Bytes image,size_t opt) {
  PEView view{image,opt};auto &process=*process_environment;
  RavenVM::code_event=+[](uintptr_t p,size_t n,bool present){process_environment->mark_ec(p,n,present);};
  auto metadata=view.metadata();
  auto mark=[&](void *base,size_t size){process.mark_ec(uintptr_t(base),size);};
  import_jit.each_region(mark);callback_jit.each_region(mark);
  process.mark_ec(uintptr_t(&raven_check_ec_call),4);
  process.mark_ec(uintptr_t(&RavenNT::raven_capture_ec_context),4);
  process.mark_ec(uintptr_t(&RavenNT::raven_restore_ec_context),4);
  auto map=image.get<uint32_t>(metadata+4),count=image.get<uint32_t>(metadata+8);
  for(uint32_t i=0;i<count;++i){auto start=image.get<uint32_t>(map+i*8),size=image.get<uint32_t>(map+i*8+4);if((start&3)==1)process.mark_ec(uintptr_t(image.data)+(start&~3u),size);}
  auto set_slot=[&](unsigned offset,void *target){auto rva=image.get<uint32_t>(metadata+offset);if(rva)image.put<uint64_t>(rva,uintptr_t(target));};
  set_slot(20,view.export_address("ExitToX64"));set_slot(24,view.export_address("RetToEntryThunk"));
  for(auto offset:{28,32,36})set_slot(offset,reinterpret_cast<void*>(&raven_check_ec_call));
  static RavenHostV1 host{};host.size=sizeof(host);host.version=1;host.shared_data=user_shared_data.data();
  auto callback=[&](auto fn){auto *adapter=native_adapter(callback_jit,reinterpret_cast<void*>(fn));mark(adapter,32);return adapter;};
  host.dispatch=reinterpret_cast<decltype(host.dispatch)>(callback(guest_dispatch));
  host.unix_call=reinterpret_cast<decltype(host.unix_call)>(callback(unix_call));
  host.exception=reinterpret_cast<decltype(host.exception)>(callback(exception_callback));
  host.cpu_area=reinterpret_cast<decltype(host.cpu_area)>(callback(cpu_area));
  host.log=reinterpret_cast<decltype(host.log)>(callback(host_log));
  host.jit_write_protect=reinterpret_cast<decltype(host.jit_write_protect)>(callback(jit_protect));
  host.allocate_native_jit=reinterpret_cast<decltype(host.allocate_native_jit)>(callback(native_jit));
  host.memory_event=reinterpret_cast<decltype(host.memory_event)>(callback(memory_event));
  auto pe=opt-24; auto sections=image.get<uint16_t>(pe+6),optional=image.get<uint16_t>(pe+20);
  for(unsigned i=0;i<sections;++i){auto section=opt+optional+i*40;auto flags=image.get<uint32_t>(section+36);if(!(flags&0x20000000))continue;
    auto rva=image.get<uint32_t>(section+12),size=image.get<uint32_t>(section+8);
    if(rva%16384)throw std::runtime_error("PE code section is not 16K aligned");
    sys_icache_invalidate(image.data+rva,size);
    if(mprotect(image.data+rva,(size+16383)&~size_t(16383),PROT_READ|PROT_EXEC))throw std::runtime_error("PE code mprotect failed");
  }
  uint64_t args[8]{uintptr_t(image.data),1,0};auto teb=thread_environment->teb.data();
  auto entry=image.get<uint32_t>(opt+16);
  if(entry){auto result=raven_ec_call(image.data+view.native_rva(entry),args,teb);if(!result)throw std::runtime_error("DllMain rejected attach");std::cout << "PASS: native PE DllMain attach\n" << std::flush;}
  args[0]=uintptr_t(&host);
  auto status=uint32_t(raven_ec_call(view.export_address("RavenProcessInit"),args,teb));
  if(status)throw std::runtime_error("RavenProcessInit returned "+std::to_string(status));
  std::cout << "PASS: RavenProcessInit\n" << std::flush;
  memset(args,0,sizeof(args));status=uint32_t(raven_ec_call(view.export_address("ThreadInit"),args,teb));
  if(status)throw std::runtime_error("ThreadInit returned "+std::to_string(status));
  std::cout << "PASS: ThreadInit\n" << std::flush;
  const uint8_t code[]{
    0xb9,7,0,0,0, 0xba,11,0,0,0, 0x41,0xb8,13,0,0,0, 0x41,0xb9,17,0,0,0,
    0x49,0x89,0xca, 0xb8,0,1,0,0, 0x0f,0x05,
    0x48,0x05,0x34,0x12,0,0, 0x49,0x89,0xc2, 0xb8,1,1,0,0, 0x0f,0x05,
    0xeb,0xfe
  };
  void *guest_code=nullptr,*guest_stack=nullptr;size_t code_size=sizeof(code),stack_size=262144;
  if(allocate(nullptr,&guest_code,0,&code_size,0x3000,0x40) || allocate(nullptr,&guest_stack,0,&stack_size,0x3000,4))throw std::runtime_error("guest allocation failed");
  memcpy(guest_code,code,sizeof(code));
  uint64_t notification[8]{uintptr_t(guest_code),code_size,0x3000,0x40,1,0};
  raven_ec_call(view.export_address("NotifyMemoryAlloc"),notification,teb);
  auto *context=thread_environment->context.data();auto rip=uintptr_t(guest_code),rsp=uintptr_t(guest_stack)+stack_size-128;
  memcpy(context+0xf8,&rip,8);memcpy(context+0x98,&rsp,8);
  expected_guest_end=rip+sizeof(code)-2;guest_calls=0;guest_ok=false;
  uint64_t previous_teb;asm volatile("mov %0,x18":"=r"(previous_teb));
  if(!sigsetjmp(guest_return,1)){
    guest_active=true;NativeJit::protect(1);
    raven_ec_call(view.export_address("BeginSimulation"),args,teb);
    throw std::runtime_error("BeginSimulation unexpectedly returned");
  }
  asm volatile("mov x18,%0"::"r"(previous_teb));guest_active=false;
  if(!guest_ok)throw std::runtime_error("guest dispatch/resume result mismatch");
  std::cout << "PASS: x86-64 dispatch args=7,11,13,17 host_result=224 resumed_result=4884 calls=1\n" << std::flush;
  RavenFault::Scope fault_scope(view.export_address("RavenHandleFault"));
  void *bad=nullptr;size_t bad_size=16384;
  if(allocate(nullptr,&bad,0,&bad_size,0x2000,1))throw std::runtime_error("fault reservation failed");
  uint8_t fault_code[]{0x48,0xb8,0,0,0,0,0,0,0,0,0x48,0x8b,0x00,0x48,0x83,0xc0,5,0x49,0x89,0xc2,0xb8,2,1,0,0,0x0f,0x05,0xeb,0xfe};
  auto bad_value=uintptr_t(bad);memcpy(fault_code+2,&bad_value,8);
  void *fault_guest=nullptr;size_t fault_size=sizeof(fault_code);
  if(allocate(nullptr,&fault_guest,0,&fault_size,0x3000,0x40))throw std::runtime_error("fault guest allocation failed");
  memcpy(fault_guest,fault_code,sizeof(fault_code));notification[0]=uintptr_t(fault_guest);notification[1]=fault_size;
  raven_ec_call(view.export_address("NotifyMemoryAlloc"),notification,teb);
  rip=uintptr_t(fault_guest);memcpy(context+0xf8,&rip,8);memcpy(context+0x98,&rsp,8);
  expected_fault_rip=rip+10;expected_fault_address=bad_value;expected_guest_end=rip+sizeof(fault_code)-2;faults_repaired=0;guest_ok=false;
  if(!sigsetjmp(guest_return,1)){guest_active=true;thread_environment->cpu.simulation=1;raven_ec_call(view.export_address("BeginSimulation"),args,teb);std::abort();}
  asm volatile("mov x18,%0"::"r"(previous_teb));guest_active=false;
  if(!guest_ok)throw std::runtime_error("precise AV repair/resume result mismatch");
  std::cout << "PASS: precise AV guest_rip=" << std::hex << expected_fault_rip << " fault_address=" << expected_fault_address << std::dec << " repaired_result=4665\n" << std::flush;
  const uint8_t callback_code[]{0x48,0x89,0xc8,0x48,0x6b,0xc0,3,0x48,0x83,0xc0,1,0x49,0x89,0xc2,0xb8,3,1,0,0,0x0f,0x05,0xeb,0xfe};
  void *callback_guest=nullptr;size_t callback_size=sizeof(callback_code);
  if(allocate(nullptr,&callback_guest,0,&callback_size,0x3000,0x40))throw std::runtime_error("callback allocation failed");
  memcpy(callback_guest,callback_code,sizeof(callback_code));notification[0]=uintptr_t(callback_guest);notification[1]=callback_size;
  raven_ec_call(view.export_address("NotifyMemoryAlloc"),notification,teb);nested_code=uintptr_t(callback_guest);
  rip=uintptr_t(guest_code);memcpy(context+0xf8,&rip,8);memcpy(context+0x98,&rsp,8);expected_guest_end=rip+sizeof(code)-2;
  exercise_nested=true;expected_resumed=5008;guest_calls=0;nested_calls=0;guest_ok=false;
  if(!sigsetjmp(guest_return,1)){guest_active=true;raven_ec_call(view.export_address("BeginSimulation"),args,teb);std::abort();}
  asm volatile("mov x18,%0"::"r"(previous_teb));guest_active=false;exercise_nested=false;
  if(!guest_ok || nested_calls!=1)throw std::runtime_error("nested callback result mismatch");
  std::cout << "PASS: nested harness-to-guest callback argument=41 result=124 outer_resumed_result=5008\n" << std::flush;
  auto install_guest=[&](const std::vector<uint8_t> &blob){void *base=nullptr;size_t size=blob.size();if(allocate(nullptr,&base,0,&size,0x3000,0x40))throw std::runtime_error("benchmark allocation failed");memcpy(base,blob.data(),blob.size());notification[0]=uintptr_t(base);notification[1]=size;raven_ec_call(view.export_address("NotifyMemoryAlloc"),notification,teb);return uintptr_t(base);};
  auto integer_code=install_guest(RavenBench::kernel(false,10000000));
  auto floating_code=install_guest(RavenBench::kernel(true,10000000));
  auto roundtrip_code=install_guest(RavenBench::roundtrip(expected_timed_calls));
  double reference=0.5;for(unsigned i=0;i<10000000;++i)reference=reference*1.0000001+0.25;
  memcpy(&float_expected_bits,&reference,8);
  auto ns=[](){timespec t{};clock_gettime(CLOCK_MONOTONIC_RAW,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;};
  auto run_timed=[&](uintptr_t address,size_t terminal,bool floating){
    memcpy(context+0xf8,&address,8);memcpy(context+0x98,&rsp,8);expected_guest_end=address+terminal;
    if(floating){double values[]{0.5,1.0000001,0.25};for(unsigned i=0;i<3;++i){memset(context+0x1a0+i*16,0,16);memcpy(context+0x1a0+i*16,&values[i],8);}}
    guest_ok=false;timed_calls=0;auto begin=ns();
    if(!sigsetjmp(guest_return,1)){guest_active=true;raven_ec_call(view.export_address("BeginSimulation"),args,teb);std::abort();}
    asm volatile("mov x18,%0"::"r"(previous_teb));guest_active=false;auto elapsed=ns()-begin;
    if(!guest_ok)throw std::runtime_error("benchmark result verification failed");return elapsed;
  };
  for(unsigned sample=0;sample<5;++sample){auto integer_ns=run_timed(integer_code,25,false);auto float_ns=run_timed(floating_code,25,true);
    std::cout << "BENCH sample=" << sample << " iterations=10000000 integer_ns=" << integer_ns << " float_ns=" << float_ns << " integer_result=" << integer_expected << " float_result=" << std::setprecision(17) << reference << "\n" << std::flush;}
  for(unsigned sample=0;sample<5;++sample){auto elapsed=run_timed(roundtrip_code,32,false);
    std::cout << "ROUNDTRIP sample=" << sample << " calls=" << expected_timed_calls << " elapsed_ns=" << elapsed << " ns_per_call=" << double(elapsed)/expected_timed_calls << "\n" << std::flush;}




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
  alignas(16) uint8_t captured[0x4d0]{};
  uint64_t capture_args[8]{uintptr_t(captured)}; uint64_t capture_teb[1024]{};
  raven_ec_call(reinterpret_cast<void*>(&RavenNT::raven_capture_ec_context),capture_args,capture_teb);
  assert(*reinterpret_cast<uint64_t*>(captured+0xf8) && *reinterpret_cast<uint64_t*>(captured+0x98));
  std::array<uint8_t,512> unwind_image{};PEView fixture_view{Bytes{unwind_image.data(),unwind_image.size()},0};
  auto *previous_image=RavenNT::active_image;RavenNT::active_image=&fixture_view;
  uint32_t unwind_header=16|(1u<<21)|(1u<<27);memcpy(unwind_image.data()+128,&unwind_header,4);
  uint8_t codes[4]{0x40,1,0xe4,0xe3};memcpy(unwind_image.data()+132,codes,4);
  uint64_t unwind_stack[2]{0xabc,0xdef};RavenNT::UnwindState state{captured};state.set(31,uintptr_t(unwind_stack));
  RavenNT::RuntimeFunction function{64,128};uint64_t establisher=0;void *handler_data=nullptr;
  assert(!RavenNT::virtual_unwind(0,uintptr_t(unwind_image.data()),uintptr_t(unwind_image.data())+96,&function,captured,&handler_data,&establisher,nullptr));
  assert(state.get(29)==0xabc && state.get(30)==0xdef && state.get(31)==uintptr_t(unwind_stack)+16);
  RavenNT::active_image=previous_image;
  RavenNT::ProcessEnvironment process;
  RavenNT::ThreadEnvironment thread(process);
  uint32_t initial_tls = 0x12345678;
  thread.install_tls(&initial_tls,4,12);
  assert(*static_cast<uint32_t*>(thread.tls_vector[0]) == initial_tls);
  assert(thread.static_tls[15]==0 && thread.cpu.context==thread.context.data());
  assert(*reinterpret_cast<void**>(thread.teb.data()+0x1788)==&thread.cpu);
  RavenNT::active_process=&process;RavenNT::active_thread=&thread;RavenNT::pseudo_handle=RavenNT::current_handle;
  auto worker_entry=+[](void *p)->uint64_t{return *static_cast<uint64_t*>(p)+1;};
  uint64_t worker_input=41;void *worker_handle=nullptr;
  assert(RavenNT::create_thread(&worker_handle,0x1fffff,nullptr,reinterpret_cast<void*>(-1),reinterpret_cast<void*>(worker_entry),&worker_input,0x10,0,0,0,0)==0);
  assert(RavenNT::wait_one(worker_handle,0,nullptr)==0);
  assert(RavenNT::get_handle(worker_handle)->control->exit_status==42);
  uint32_t object_info[14]{};assert(RavenNT::query_object(worker_handle,0,object_info,sizeof(object_info),nullptr)==0 && object_info[1]==0x1fffff);
  assert(RavenNT::close_handle(worker_handle)==0);
  RavenNT::String registry_name{};RavenNT::init_unicode(&registry_name,u"\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0");
  RavenNT::ObjectAttributes registry_object{48,0,nullptr,&registry_name,0,0,nullptr,nullptr};void *registry_key=nullptr;
  assert(RavenNT::open_key(&registry_key,0,&registry_object,0)==0);
  RavenNT::String value_name{};RavenNT::init_unicode(&value_name,u"CP 4030");uint8_t registry_value[20]{};
  assert(RavenNT::query_value(registry_key,&value_name,2,registry_value,20,nullptr)==0);assert(RavenNT::close_handle(registry_key)==0);
  RavenNT::active_process=nullptr;RavenNT::active_thread=nullptr;
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
  if (argc==3 && std::string(argv[1])=="--execute") return map_module(argc-1,argv+1,resolve,inspect_image,execute_image);
  return map_module(argc, argv, resolve, inspect_image);
}
