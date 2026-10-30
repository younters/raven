// SPDX-License-Identifier: MIT
#pragma once
#include "nt_system.h"
#include <setjmp.h>
namespace RavenNT {
struct ThreadControl {
  pthread_t pthread{};std::mutex mutex;std::condition_variable condition;
  bool done=false,ready=false;uint64_t tid=0;int32_t exit_status=0;
  std::unique_ptr<ThreadEnvironment> environment;
};
thread_local std::shared_ptr<Handle> self_handle;
thread_local jmp_buf *thread_exit_frame=nullptr;
std::shared_ptr<Handle> current_handle(void *) {
  if(!self_handle){self_handle=std::make_shared<Handle>(Handle::thread);self_handle->access=0x1fffff;self_handle->control=std::make_shared<ThreadControl>();self_handle->control->pthread=pthread_self();pthread_threadid_np(nullptr,&self_handle->control->tid);}
  return self_handle;
}
struct Start {std::shared_ptr<Handle> handle;void *function,*argument;};
void *thread_start(void *opaque) {
  std::unique_ptr<Start> start(static_cast<Start*>(opaque));auto h=start->handle;
  self_handle=h;auto c=h->control;pthread_threadid_np(nullptr,&c->tid);
  c->environment=std::make_unique<ThreadEnvironment>(*active_process);active_thread=c->environment.get();
  {std::lock_guard<std::mutex> guard(c->mutex);c->ready=true;}c->condition.notify_all();
  jmp_buf exit;thread_exit_frame=&exit;
  if(!setjmp(exit)){uint64_t args[8]{uintptr_t(start->argument)};c->exit_status=raven_ec_call(start->function,args,active_thread->teb.data());}
  thread_exit_frame=nullptr;
  {std::lock_guard<std::mutex> guard(c->mutex);c->done=true;}c->condition.notify_all();
  self_handle.reset();active_thread=nullptr;return nullptr;
}
int32_t create_thread(void **out,uint32_t access,ObjectAttributes *attributes,void *process,void *function,void *arg,uint32_t flags,uint64_t zero,uint64_t stack,uint64_t reserve,uint64_t list) {
  if(out)*out=nullptr;if(!out || !function || !active_process)return invalid_parameter;
  if(intptr_t(process)!=-1 || attributes || zero || list || (flags&~0x56u))return not_supported;
  auto h=std::make_shared<Handle>(Handle::thread);h->access=access;h->control=std::make_shared<ThreadControl>();
  auto start=std::make_unique<Start>(Start{h,function,arg});pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
  auto size=std::max(stack,reserve);if(size && pthread_attr_setstacksize(&attr,std::max<uint64_t>(size,PTHREAD_STACK_MIN))){pthread_attr_destroy(&attr);return invalid_parameter;}
  int error=pthread_create(&h->control->pthread,&attr,thread_start,start.get());pthread_attr_destroy(&attr);if(error)return os_error(error);
  start.release();{std::unique_lock<std::mutex> lock(h->control->mutex);h->control->condition.wait(lock,[&]{return h->control->ready;});}*out=new_handle(h);return 0;
}
int32_t wait_one(void *handle,uint8_t alert,const int64_t *timeout) {
  if(alert)return not_supported;auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  auto c=h->control;std::unique_lock<std::mutex> lock(c->mutex);
  return c->condition.wait_until(lock,RavenSync::deadline(timeout),[&]{return c->done;})?0:0x102;
}
[[noreturn]] void terminate_process(void *process,int32_t status) {
  if(intptr_t(process)!=-1){fprintf(stderr,"invalid process termination handle\n");std::abort();}
  fprintf(stderr,"module requested process termination status=%08x\n",uint32_t(status));fflush(stderr);_exit(status);
}
int32_t terminate_thread(void *handle,int32_t status) {
  auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  if(!pthread_equal(h->control->pthread,pthread_self()))return not_supported;
  if(!thread_exit_frame)return not_supported;
  h->control->exit_status=status;longjmp(*thread_exit_frame,1);
}
int32_t suspend_thread(void *handle,uint32_t *previous) {
  auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  auto port=pthread_mach_thread_np(h->control->pthread);thread_basic_info_data_t info{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
  if(thread_info(port,THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&info),&n)!=KERN_SUCCESS)return int32_t(0xc0000001);
  if(thread_suspend(port)!=KERN_SUCCESS)return int32_t(0xc0000022);
  if(previous)*previous=info.suspend_count;return 0;
}
int32_t query_object(void *handle,uint32_t klass,void *out,uint32_t length,uint32_t *written) {
  if(written)*written=0;auto h=get_handle(handle);if(!h)return bad_handle;
  if(klass!=0)return int32_t(0xc0000003);
  if(written)*written=56;if(!out || length<56)return int32_t(0xc0000004);
  memset(out,0,56);auto *fields=static_cast<uint32_t*>(out);fields[1]=h->access;
  {std::lock_guard<std::mutex> guard(handles_mutex);for(const auto &p:handles)if(p.second==h)++fields[2];}
  fields[3]=h.use_count();return 0;
}
int32_t query_thread_handle(void *handle,uint32_t klass,void *out,uint32_t length,uint32_t *written) {
  if(written)*written=0;auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  if(klass!=0)return int32_t(0xc0000003);if(written)*written=48;
  if(!out || length<48)return int32_t(0xc0000004);
  auto c=h->control;std::lock_guard<std::mutex> guard(c->mutex);
  auto *environment=pthread_equal(c->pthread,pthread_self())?active_thread:c->environment.get();
  if(!environment)return not_supported;
  struct Basic {int32_t status,pad;void *teb;uint64_t pid,tid,mask;int32_t priority,base;};
  auto n=sysconf(_SC_NPROCESSORS_ONLN);uint64_t mask=n>=64?UINT64_MAX:(1ull<<n)-1;
  *static_cast<Basic*>(out)={c->done?c->exit_status:0x103,0,environment->teb.data(),uint64_t(getpid()),c->tid,mask,0,0};return 0;
}
int32_t set_thread_handle(void *handle,uint32_t klass,const void *in,uint32_t length) {
  auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  if(!pthread_equal(h->control->pthread,pthread_self()))return not_supported;
  return set_thread(reinterpret_cast<void*>(-2),klass,in,length);
}
void *resolve_threads(const char *name,unsigned &stack) {
#define NT(n,f,s) if(!strcmp(name,n)){stack=s;return reinterpret_cast<void*>(&f);}
  NT("NtQueryInformationThread",query_thread_handle,0);NT("NtSetInformationThread",set_thread_handle,0);
  NT("NtCreateThreadEx",create_thread,3);NT("NtWaitForSingleObject",wait_one,0);NT("NtTerminateProcess",terminate_process,0);
  NT("NtTerminateThread",terminate_thread,0);NT("NtSuspendThread",suspend_thread,0);NT("NtQueryObject",query_object,0);
#undef NT
  return nullptr;
}
}
