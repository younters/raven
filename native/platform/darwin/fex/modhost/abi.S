// SPDX-License-Identifier: MIT
// Native arm64 -> ARM64EC. Uniform eight integer arguments, passed as an array.
// EC may destroy x23, x24, x28; Darwin requires them to survive.
.text
.p2align 2
.globl _raven_ec_call
_raven_ec_call:
  stp x29, x30, [sp, #-176]!
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  str x18, [sp, #160]
  mov x16, x0
  mov x18, x2
  mov x17, x1
  ldp x0, x1, [x17]
  ldp x2, x3, [x17, #16]
  ldp x4, x5, [x17, #32]
  ldp x6, x7, [x17, #48]
  blr x16
  ldr x18, [sp, #160]
  ldp d14, d15, [sp, #144]
  ldp d12, d13, [sp, #128]
  ldp d10, d11, [sp, #112]
  ldp d8, d9, [sp, #96]
  ldp x27, x28, [sp, #80]
  ldp x25, x26, [sp, #64]
  ldp x23, x24, [sp, #48]
  ldp x21, x22, [sp, #32]
  ldp x19, x20, [sp, #16]
  ldp x29, x30, [sp], #176
  ret

// EC -> native arm64. A per-import stub puts the native wrapper in x16.
// Uniform uint64_t parameters: up to eight in registers, eight on stack.
// Wrapper signatures must use uint64_t for ALL integer arguments: Darwin packs
// narrower stack values whereas Windows reserves eight bytes per parameter.
// Nonvariadic FP arguments in d0-d7 and results in d0 pass through untouched.
.p2align 2
.globl _raven_native_call
_raven_native_call:
  sub sp, sp, #256
  stp x29, x30, [sp, #64]
  str x18, [sp, #80]
  stp q6, q7, [sp, #96]
  stp q8, q9, [sp, #128]
  stp q10, q11, [sp, #160]
  stp q12, q13, [sp, #192]
  stp q14, q15, [sp, #224]
  // x17 is the exact number of eight-byte stack arguments (0..8).
  cbz x17, 2f
  add x9, sp, #256
  mov x10, sp
1:
  ldr x11, [x9], #8
  str x11, [x10], #8
  subs x17, x17, #1
  b.ne 1b
2:
  blr x16
  ldp q14, q15, [sp, #224]
  ldp q12, q13, [sp, #192]
  ldp q10, q11, [sp, #160]
  ldp q8, q9, [sp, #128]
  ldp q6, q7, [sp, #96]
  ldr x18, [sp, #80]
  ldp x29, x30, [sp, #64]
  add sp, sp, #256
  ret
