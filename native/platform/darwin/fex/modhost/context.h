// SPDX-License-Identifier: MIT
#pragma once
#include "nt_threads.h"
#include <deque>
namespace RavenNT {
// ARM64EC_NT_CONTEXT mapped GPR offsets in the pinned Windows ABI.
constexpr uint16_t ec_offsets[31]{0x80,0x88,0xb8,0xc0,0xc8,0xd0,0x130,0x140,0x78,0x150,0x160,0x170,0x180,0,0,0x190,0,0,0,0xd8,0xe0,0xe8,0xf0,0,0,0xa8,0xb0,0x90,0,0xa0,0x120};
void pack_context(void *out,const arm_thread_state64_t &state,const arm_neon_state64_t &neon) {
  auto *p=static_cast<uint8_t*>(out);
  for(unsigned i=0;i<29;++i)if(ec_offsets[i])memcpy(p+ec_offsets[i],&state.__x[i],8);
  auto fp=arm_thread_state64_get_fp(state),lr=arm_thread_state64_get_lr(state),sp=arm_thread_state64_get_sp(state),pc=arm_thread_state64_get_pc(state);
  memcpy(p+0xa0,&fp,8);memcpy(p+0x120,&lr,8);memcpy(p+0x98,&sp,8);memcpy(p+0xf8,&pc,8);
  for(unsigned i=0;i<16;++i)memcpy(p+0x1a0+i*16,&neon.__v[i],16);
  uint32_t flags=0x10000b,eflags=0x202;
  eflags|=(state.__cpsr&(1u<<31))?0x80:0;eflags|=(state.__cpsr&(1u<<30))?0x40:0;
  eflags|=(state.__cpsr&(1u<<29))?1:0;eflags|=(state.__cpsr&(1u<<28))?0x800:0;
  memcpy(p+0x30,&flags,4);memcpy(p+0x44,&eflags,4);
}
int32_t get_context(void *handle,void *out) {
  if(!out)return invalid_parameter;auto h=get_handle(handle);if(!h || h->kind!=Handle::thread)return bad_handle;
  if(pthread_equal(h->control->pthread,pthread_self()))return int32_t(0xc0000001);
  auto port=pthread_mach_thread_np(h->control->pthread);arm_thread_state64_t state{};arm_neon_state64_t neon{};
  mach_msg_type_number_t n=ARM_THREAD_STATE64_COUNT;
  if(thread_get_state(port,ARM_THREAD_STATE64,reinterpret_cast<thread_state_t>(&state),&n)!=KERN_SUCCESS)return int32_t(0xc0000001);
  n=ARM_NEON_STATE64_COUNT;if(thread_get_state(port,ARM_NEON_STATE64,reinterpret_cast<thread_state_t>(&neon),&n)!=KERN_SUCCESS)return int32_t(0xc0000001);
  pack_context(out,state,neon);return 0;
}
uint8_t is_ec(void *address) {
  if(!active_process || uintptr_t(address)>=(1ull<<47))return 0;
  auto page=uintptr_t(address)>>12;
  return (__atomic_load_n(static_cast<uint8_t*>(active_process->code_bitmap)+page/8,__ATOMIC_ACQUIRE)>>(page%8))&1;
}
void *extended_feature(void *extended,uint32_t feature,uint32_t *length) {
  if(length)*length=0;if(!extended || feature!=2)return nullptr;
  int32_t offset;uint32_t n;memcpy(&offset,static_cast<char*>(extended)+16,4);memcpy(&n,static_cast<char*>(extended)+20,4);
  if(n<64+256)return nullptr;if(length)*length=256;return static_cast<char*>(extended)+offset+64;
}
std::mutex pending_mutex;
std::deque<std::pair<void(*)(void*),void*>> pending;
void process_pending() {
  while(true){std::pair<void(*)(void*),void*> work;
    {std::lock_guard<std::mutex> guard(pending_mutex);if(pending.empty())return;work=pending.front();pending.pop_front();}
    work.first(work.second);
  }
}
void *resolve_context(const char *name) {
#define NT(n,f) if(!strcmp(name,n))return reinterpret_cast<void*>(&f)
  NT("NtGetContextThread",get_context);NT("RtlIsEcCode",is_ec);NT("RtlLocateExtendedFeature",extended_feature);NT("ProcessPendingCrossProcessEmulatorWork",process_pending);
#undef NT
  return nullptr;
}
}
