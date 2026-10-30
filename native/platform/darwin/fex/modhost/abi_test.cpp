// SPDX-License-Identifier: MIT
#include "abi.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
static void check(bool value) { if (!value) { fprintf(stderr, "ABI/JIT check failed\n"); std::abort(); } }
static uint64_t sum(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                    uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h;
}
extern "C" void raven_abi_fixture();
static uint64_t sum16(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                      uint64_t e, uint64_t f, uint64_t g, uint64_t h,
                      uint64_t i, uint64_t j, uint64_t k, uint64_t l,
                      uint64_t m, uint64_t n, uint64_t o, uint64_t p) {
  return sum(a,b,c,d,e,f,g,h) + 9*i + 10*j + 11*k + 12*l + 13*m + 14*n + 15*o + 16*p;
}
int main() {
  check(getpagesize() == 16384);
  NativeJit jit;
  check(NativeJit::protect(2) == 0);
  auto adapter = native_adapter(jit, reinterpret_cast<void *>(&sum));
  uint64_t args[8] {1,2,3,4,5,6,7,8};
  uint64_t teb[1024] {};
  check(raven_ec_call(adapter, args, teb) == 204);
  check(NativeJit::protect(0) == 1);
  check(raven_ec_call(adapter, args, teb) == 204);
  check(NativeJit::protect(0) == 0);
  check(NativeJit::protect(1) == 0);
  check(NativeJit::protect(1) == 1);
  auto adapter16 = native_adapter(jit, reinterpret_cast<void *>(&sum16), 8);
  args[0] = reinterpret_cast<uintptr_t>(adapter16);
  check(raven_ec_call(reinterpret_cast<void *>(&raven_abi_fixture), args, teb) == 1496);
  puts("PASS: MAP_JIT execution, nested write modes, sixteen integer arguments and EC register clobbers");
}
