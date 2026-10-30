// SPDX-License-Identifier: MIT
#pragma once
#include "environment.h"
#include "nt_io.h"
#include "sync.h"
#include "abi.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sched.h>
#include <thread>
namespace RavenNT {
ProcessEnvironment *active_process=nullptr;
thread_local ThreadEnvironment *active_thread=nullptr;
PEView *active_image=nullptr;
char ntdll_token;
void *(*import_lookup)(const char *,const char *)=nullptr;
int32_t full_name(void *module,String *out) {
  if(!active_process || !out)return invalid_parameter;
  if(module && (!active_image || module!=active_image->bytes.data))return bad_handle;
  auto &name=active_process->image_path;auto n=name.size()*2;
  if(out->maximum<n+2)return buffer_small;
  memcpy(out->buffer,name.data(),n);static_cast<char16_t*>(out->buffer)[n/2]=0;out->length=n;return 0;
}
int32_t dll_handle(void *,uint32_t,String *name,void **out) {
  if(!out)return invalid_parameter;*out=nullptr;
  if(!name) {if(active_image){*out=active_image->bytes.data;return 0;}return int32_t(0xc0000135);}
  std::string n=encode(static_cast<char16_t*>(name->buffer),name->length/2);
  std::transform(n.begin(),n.end(),n.begin(),[](unsigned char c){return tolower(c);});
  if(n=="ntdll.dll") {*out=&ntdll_token;return 0;}
  if(n=="xtajit64.dll" && active_image){*out=active_image->bytes.data;return 0;}
  return int32_t(0xc0000135);
}
int32_t procedure(void *module,String *name,uint32_t ordinal,void **out) {
  if(!out)return invalid_parameter;*out=nullptr;
  if(!name || ordinal)return int32_t(0xc000007a);
  std::string n(static_cast<char*>(name->buffer),name->length);
  if(module==&ntdll_token && import_lookup) *out=import_lookup("ntdll.dll",n.c_str());
  else if(active_image && module==active_image->bytes.data) *out=active_image->export_address(n.c_str());
  else return bad_handle;
  return *out?0:int32_t(0xc000007a);
}
int32_t disable_callouts(void *module) {
  // No host loader thread callouts are registered for this sole module.
  // Static TLS callbacks belong to explicit CRT init, not loader notifications.
  return active_image && module==active_image->bytes.data ? 0 : bad_handle;
}
void *image_nt_header(void *base) {
  if(!active_image || base!=active_image->bytes.data)return nullptr;
  return active_image->bytes.data+active_image->opt-24;
}
void *image_directory(void *base,uint8_t mapped,uint16_t index,uint32_t *size) {
  if(size)*size=0;
  if(!active_image || base!=active_image->bytes.data || !mapped || index>=16)return nullptr;
  auto at=active_image->directory(index);if(!at)return nullptr;
  auto n=active_image->bytes.get<uint32_t>(active_image->opt+116+index*8);
  active_image->bytes.check(at,n);if(size)*size=n;return active_image->bytes.data+at;
}
int32_t system_info(uint32_t klass,void *out,uint32_t length,uint32_t *written) {
  if(written)*written=0;
  if(klass!=0 && klass!=62)return int32_t(0xc0000003);
  struct Basic {uint32_t reserved,timer,page,physical,lowest,highest,granularity,pad;uint64_t low,high,mask;uint8_t count;uint8_t padding[7];};
  static_assert(sizeof(Basic)==64);
  if(written)*written=64;if(!out || length<64)return int32_t(0xc0000004);
  uint64_t physical=0;size_t n=8;sysctlbyname("hw.memsize",&physical,&n,nullptr,0);
  auto count=uint32_t(sysconf(_SC_NPROCESSORS_ONLN));
  *static_cast<Basic*>(out)={0,10000,uint32_t(getpagesize()),uint32_t(physical/getpagesize()),0,uint32_t(physical/getpagesize()-1),65536,0,0x100000000ull,0x7fffffffffull,count>=64?UINT64_MAX:(1ull<<count)-1,uint8_t(count),{}};return 0;
}
int32_t system_time(int64_t *out) {if(!out)return invalid_parameter;timespec now{};if(clock_gettime(CLOCK_REALTIME,&now))return os_error(errno);*out=nt_time(now);return 0;}
uint8_t performance_frequency(int64_t *out) {if(!out)return 0;mach_timebase_info_data_t t{};mach_timebase_info(&t);*out=1000000000ull*t.denom/t.numer;return 1;}
int32_t delay(uint8_t alert,const int64_t *time) {if(alert)return not_supported;if(!time)return invalid_parameter;auto until=RavenSync::deadline(time);std::this_thread::sleep_until(until);return 0;}
uint32_t processor() {
  size_t cpu=0;
  if (pthread_cpu_number_np(&cpu)) std::abort();
  return uint32_t(cpu);
}
uint32_t dos_error(int32_t status) {
  switch(uint32_t(status)){case 0:return 0;case 0xc0000034:return 2;case 0xc000003a:return 3;case 0xc0000022:return 5;case 0xc0000008:return 6;case 0xc0000017:return 8;case 0xc000000d:return 87;case 0xc00000bb:return 50;case 0xc0000023:case 0x80000005:return 122;case 0xc0000035:return 183;case 0xc0000135:return 126;case 0xc000007a:return 127;case 0xc0000011:return 38;default:return 317;}
}
int32_t query_thread(void *handle,uint32_t klass,void *out,uint32_t length,uint32_t *written) {
  if(written)*written=0;if(intptr_t(handle)!=-2 || !active_thread)return bad_handle;
  if(klass!=0)return int32_t(0xc0000003);
  struct Basic {int32_t status,pad;void *teb;uint64_t pid,tid,affinity;int32_t priority,base_priority;};
  if(written)*written=48;if(!out || length<48)return int32_t(0xc0000004);
  uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
  *static_cast<Basic*>(out)={0,0,active_thread->teb.data(),uint64_t(getpid()),tid,UINT64_MAX,0,0};return 0;
}
int32_t set_thread(void *handle,uint32_t klass,const void *in,uint32_t length) {
  if(intptr_t(handle)!=-2 || !active_thread)return bad_handle;
  if(klass==10 && length==4) {auto slot=*static_cast<const uint32_t*>(in);if(slot>=64)return invalid_parameter;active_thread->put<void*>(0x1480+slot*8,nullptr);return 0;}
  if(klass==3 && length==4) {
    auto priority=*static_cast<const int32_t*>(in);
    if(priority==-15)return os_error(pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0));
    if(priority==0)return os_error(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT,0));
    return not_supported;
  }
  return int32_t(0xc0000003);
}
struct Once {std::mutex mutex;std::condition_variable condition;bool running=false,done=false;void *context=nullptr;};
std::mutex once_mutex;std::map<void*,std::unique_ptr<Once>> once_objects;
int32_t run_once(void *key,void *callback,void *parameter,void **context) {
  Once *once;
  {std::lock_guard<std::mutex> guard(once_mutex);auto &p=once_objects[key];if(!p)p=std::make_unique<Once>();once=p.get();}
  std::unique_lock<std::mutex> lock(once->mutex);while(once->running)once->condition.wait(lock);
  if(once->done){if(context)*context=once->context;return 0;}
  if(!active_thread)return invalid_parameter;
  once->running=true;lock.unlock();void *value=nullptr;
  uint64_t args[8]{uintptr_t(key),uintptr_t(parameter),uintptr_t(&value)};
  auto success=raven_ec_call(callback,args,active_thread->teb.data());
  lock.lock();once->running=false;if(success){once->done=true;once->context=value;}lock.unlock();once->condition.notify_all();
  if(context)*context=value;return success?0:int32_t(0xc0000001);
}
void *resolve_system(const char *name) {
#define NT(n,f) if(!strcmp(name,n))return reinterpret_cast<void*>(&f)
  NT("LdrGetDllFullName",full_name);NT("LdrGetDllHandle",dll_handle);NT("LdrGetProcedureAddress",procedure);NT("LdrDisableThreadCalloutsForDll",disable_callouts);
  NT("RtlImageNtHeader",image_nt_header);NT("RtlImageDirectoryEntryToData",image_directory);
  NT("NtQuerySystemInformation",system_info);NT("NtQuerySystemTime",system_time);NT("RtlQueryPerformanceFrequency",performance_frequency);
  NT("NtDelayExecution",delay);NT("NtGetCurrentProcessorNumber",processor);NT("RtlNtStatusToDosError",dos_error);
  NT("NtQueryInformationThread",query_thread);NT("NtSetInformationThread",set_thread);NT("RtlRunOnceExecuteOnce",run_once);
#undef NT
  return nullptr;
}
}
