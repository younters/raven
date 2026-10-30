// SPDX-License-Identifier: MIT
#pragma once
#include "jit.h"
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>
namespace RavenVM {
constexpr int32_t invalid=int32_t(0xc000000d), unsupported=int32_t(0xc00000bb), no_memory=int32_t(0xc0000017);
size_t page() { return getpagesize(); }
size_t round(size_t n) { return n && n<=SIZE_MAX-page()+1 ? (n+page()-1)&~(page()-1) : 0; }
int prot(uint32_t p,bool ec=false) {
  // Guest executable pages contain x86, not native instructions.
  switch(p) { case 1:return PROT_NONE; case 2:case 0x20:return PROT_READ;
    case 4:case 8:case 0x40:case 0x80:return PROT_READ|PROT_WRITE|(ec?PROT_EXEC:0);
    case 0x10:return ec?PROT_EXEC:PROT_READ; default:return -1; }
}
struct Region { size_t size; uint32_t initial; bool ec; std::vector<uint32_t> protections; };
std::mutex mutex;
std::map<uintptr_t,Region> regions;
auto containing(uintptr_t p) {
  auto i=regions.upper_bound(p); if(i==regions.begin()) return regions.end(); --i;
  return p-i->first<i->second.size ? i : regions.end();
}
int32_t allocate_impl(void *process,void **base,size_t *size,uint32_t type,uint32_t protection,bool ec) {
  if(process && intptr_t(process)!=-1) return int32_t(0xc0000008);
  if(!base || !size || !*size || prot(protection,ec)<0) return invalid;
  if(type&~0x103000u || !(type&0x3000)) return unsupported;
  uintptr_t at=uintptr_t(*base)&~(page()-1); auto offset=uintptr_t(*base)-at;
  if(*size>SIZE_MAX-offset) return invalid;
  auto n=round(*size+offset); if(!n) return invalid;
  std::lock_guard<std::mutex> guard(mutex);
  if(*base && !(type&0x2000)) {
    auto i=containing(at);
    if(i==regions.end() || n>i->second.size-(at-i->first)) return invalid;
    if(mprotect(reinterpret_cast<void*>(at),n,prot(protection,i->second.ec))) return invalid;
    auto first=(at-i->first)/page();
    std::fill_n(i->second.protections.begin()+first,n/page(),protection);
    *base=reinterpret_cast<void*>(at); *size=n; return 0;
  }
  if(ec && !(type&0x1000)) return unsupported;
  int flags=MAP_PRIVATE|MAP_ANON|(ec?MAP_JIT:0);
  auto *p=mmap(reinterpret_cast<void*>(at),n,type&0x1000?prot(protection,ec):PROT_NONE,flags,-1,0);
  if(p==MAP_FAILED) return no_memory;
  if(at && uintptr_t(p)!=at) { munmap(p,n); return int32_t(0xc0000018); }
  if(getenv("RAVEN_MODHOST_TRACE"))fprintf(stderr,"VM alloc %p size=%zu EC=%d type=%x protect=%x\n",p,n,ec,type,protection);
  regions.emplace(uintptr_t(p),Region{n,protection,ec,std::vector<uint32_t>(n/page(),type&0x1000?protection:0)});
  *base=p; *size=n; return 0;
}
int32_t allocate(void *p,void **b,uintptr_t zero,size_t *s,uint32_t t,uint32_t prot) {
  if(zero) return unsupported; return allocate_impl(p,b,s,t,prot,false);
}
struct Extended { uint64_t type,value; };
int32_t allocate_ex(void *p,void **b,size_t *s,uint32_t t,uint32_t prot,const Extended *extra,uint32_t count) {
  bool ec=false;
  if(count && !extra) return invalid;
  for(uint32_t i=0;i<count;++i) {
    if(extra[i].type==5 && !(extra[i].value&~0x40ull)) ec=extra[i].value&0x40;
    else return unsupported;
  }
  return allocate_impl(p,b,s,t,prot,ec);
}
int32_t release(void *p,void **base,size_t *size,uint32_t type) {
  if(p && intptr_t(p)!=-1) return int32_t(0xc0000008);
  if(!base || !size) return invalid;
  std::lock_guard<std::mutex> guard(mutex);
  auto at=uintptr_t(*base); auto i=containing(at);
  if(i==regions.end()) return invalid;
  if(type==0x8000) {
    if(at!=i->first || *size) return invalid;
    if(munmap(*base,i->second.size)) return invalid;
    regions.erase(i); *base=nullptr; return 0;
  }
  if(type!=0x4000) return unsupported;
  auto aligned=at&~(page()-1); auto n=round(*size+at-aligned);
  if(!n || n>i->second.size-(aligned-i->first)) return invalid;
  if(i->second.ec) {
    if(mprotect(reinterpret_cast<void*>(aligned),n,PROT_READ|PROT_WRITE|PROT_EXEC)) return invalid;
    auto previous=NativeJit::protect(0);memset(reinterpret_cast<void*>(aligned),0,n);NativeJit::protect(previous);
    if(mprotect(reinterpret_cast<void*>(aligned),n,PROT_NONE)) return invalid;
  } else if(mmap(reinterpret_cast<void*>(aligned),n,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED) return no_memory;
  std::fill_n(i->second.protections.begin()+(aligned-i->first)/page(),n/page(),0);
  *base=reinterpret_cast<void*>(aligned); *size=n; return 0;
}
int32_t protect(void *p,void **base,size_t *size,uint32_t protection,uint32_t *old) {
  if(p && intptr_t(p)!=-1) return int32_t(0xc0000008);
  if(!base || !size || !old || !*size) return invalid;
  auto at=uintptr_t(*base)&~(page()-1), n=round(*size+uintptr_t(*base)-at);
  std::lock_guard<std::mutex> guard(mutex); auto i=containing(at);
  if(i==regions.end() || !n || n>i->second.size-(at-i->first) || prot(protection,i->second.ec)<0) return invalid;
  auto first=(at-i->first)/page();
  for(size_t j=0;j<n/page();++j) if(!i->second.protections[first+j]) return int32_t(0xc000002d);
  if(mprotect(reinterpret_cast<void*>(at),n,prot(protection,i->second.ec))) return invalid;
  if(getenv("RAVEN_MODHOST_TRACE"))fprintf(stderr,"VM protect %p size=%zu EC=%d protect=%x\n",reinterpret_cast<void*>(at),n,i->second.ec,protection);
  *old=i->second.protections[first]; std::fill_n(i->second.protections.begin()+first,n/page(),protection);
  *base=reinterpret_cast<void*>(at); *size=n; return 0;
}
struct MemoryInfo { void *base,*allocation_base; uint32_t allocation_protect,pad; uint64_t region_size; uint32_t state,protect,type,pad2; };
static_assert(sizeof(MemoryInfo)==48);
int32_t query(void *p,void *address,uint32_t klass,void *output,size_t length,size_t *written) {
  if(written) *written=0;
  if(p && intptr_t(p)!=-1) return int32_t(0xc0000008);
  if(klass || !output || length<sizeof(MemoryInfo)) return invalid;
  std::lock_guard<std::mutex> guard(mutex); auto at=uintptr_t(address)&~(page()-1); auto i=containing(at);
  if(i==regions.end()) return invalid;
  auto first=(at-i->first)/page(), end=first+1; auto value=i->second.protections[first];
  while(end<i->second.protections.size() && i->second.protections[end]==value) ++end;
  *static_cast<MemoryInfo*>(output)={reinterpret_cast<void*>(at),reinterpret_cast<void*>(i->first),i->second.initial,0,(end-first)*page(),value?0x1000u:0x2000u,value,0x20000,0};
  if(written) *written=48;
  return 0;
}
int32_t flush(void *p,void *base,size_t size) {
  if(p && intptr_t(p)!=-1) return int32_t(0xc0000008);
  if(!base && size) return invalid;
  if(size) sys_icache_invalidate(base,size);
  return 0;
}
}
