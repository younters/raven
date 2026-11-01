#include "unix_guard.h"
#include "fex/unix_ranges.h"
#include "dxmt/checked_memory.h"
#include <chrono>
#include <cstdio>
static UnixRanges ranges;
static int check(uint64_t p,uint64_t n,uint32_t w) { return ranges.check(p,n,w); }
static int valid(uint32_t,void *,raven_unix_check_range) { return 1; }
static void set(raven_unix_check_range) {}
static const uint32_t sizes[]={16};
static const raven_unix_contract contract={1,1,sizes,valid,set};
struct Args { void *dst, *src; };
__attribute__((noinline)) static int32_t copy(void *p) { auto *a=(Args *)p; raven_unix_copy(a->dst,a->src,16); return 0; }
__attribute__((noinline)) static int32_t empty(void *p) { asm volatile(""::"r"(p):"memory"); return 0; }
int main() {
  alignas(16384) char data[16384]{};
  Args *args=(Args *)data; args->dst=data+64; args->src=data+128;
  ranges.set((uintptr_t)data,sizeof data,3); raven_unix_set_memory_check(check);
  for (auto function : {empty,copy}) {
    for (unsigned sample=0;sample<3;++sample) {
      auto start=std::chrono::steady_clock::now();
      for (unsigned i=0;i<200000;++i) if(raven_unix_invoke(function,args,0,&contract,check)) return 1;
      double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/200000;
      printf("%s %u %.2f ns/call\n",function==empty?"empty":"copy16",sample,ns);
    }
  }
}
