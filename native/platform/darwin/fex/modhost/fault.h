// SPDX-License-Identifier: MIT
#pragma once
#include "context.h"
#include <signal.h>
#include <sys/ucontext.h>
namespace RavenFault {
struct Record {uint32_t code,flags;void *chained,*address;uint32_t count,pad;uint64_t information[15];};
struct alignas(16) NativeContext {uint32_t flags,cpsr;uint64_t x[31],sp,pc;__uint128_t v[32];uint32_t fpcr,fpsr;uint8_t debug[120];};
static_assert(sizeof(NativeContext)==0x390 && offsetof(NativeContext,v)==0x110);
void *entry=nullptr;
alignas(16) uint8_t signal_stack[128*1024];
void handler(int signal,siginfo_t *info,void *opaque) {
  auto *uc=static_cast<ucontext_t*>(opaque);auto &ss=uc->uc_mcontext->__ss;auto &ns=uc->uc_mcontext->__ns;
  NativeContext native{};native.flags=0x400007;native.cpsr=ss.__cpsr;
  for(unsigned i=0;i<29;++i)native.x[i]=ss.__x[i];
  native.x[29]=arm_thread_state64_get_fp(ss);native.x[30]=arm_thread_state64_get_lr(ss);
  native.sp=arm_thread_state64_get_sp(ss);native.pc=arm_thread_state64_get_pc(ss);
  memcpy(native.v,ns.__v,sizeof(native.v));native.fpcr=ns.__fpcr;native.fpsr=ns.__fpsr;
  Record record{};record.code=0xc0000005;record.address=reinterpret_cast<void*>(native.pc);record.count=2;
  record.information[0]=(uc->uc_mcontext->__es.__esr>>6)&1;record.information[1]=uintptr_t(info->si_addr);
  if(entry && RavenNT::active_thread){uint64_t args[8]{uintptr_t(&record),uintptr_t(&native)};
    auto status=uint32_t(raven_ec_call(entry,args,RavenNT::active_thread->teb.data()));
    if(!status){for(unsigned i=0;i<29;++i)ss.__x[i]=native.x[i];
      arm_thread_state64_set_fp(ss,native.x[29]);arm_thread_state64_set_lr_fptr(ss,reinterpret_cast<void(*)()>(native.x[30]));
      arm_thread_state64_set_sp(ss,native.sp);arm_thread_state64_set_pc_fptr(ss,reinterpret_cast<void(*)()>(native.pc));
      ss.__cpsr=native.cpsr;memcpy(ns.__v,native.v,sizeof(native.v));ns.__fpcr=native.fpcr;ns.__fpsr=native.fpsr;
      return;
    }
  }
  const char error[]="unhandled standalone FEX memory fault\n";write(2,error,sizeof(error)-1);_exit(128+signal);
}
struct Scope {
  struct sigaction previous_segv{},previous_bus{};stack_t previous_stack{};
  explicit Scope(void *fault_entry){entry=fault_entry;if(!entry)throw std::runtime_error("missing RavenHandleFault export");
    stack_t stack{};stack.ss_sp=signal_stack;stack.ss_size=sizeof(signal_stack);
    if(sigaltstack(&stack,&previous_stack))throw std::runtime_error("sigaltstack failed");
    struct sigaction action{};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&action.sa_mask);
    if(sigaction(SIGSEGV,&action,&previous_segv) || sigaction(SIGBUS,&action,&previous_bus))throw std::runtime_error("sigaction failed");
  }
  ~Scope(){sigaction(SIGSEGV,&previous_segv,nullptr);sigaction(SIGBUS,&previous_bus,nullptr);sigaltstack(&previous_stack,nullptr);entry=nullptr;}
};
}
