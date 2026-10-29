// SPDX-License-Identifier: MIT
.text
.p2align 2
// Test-only EC-shaped callee. Deliberately destroys EC-volatile/Darwin-saved
// registers, then calls the import adapter with sixteen integer arguments.
.globl _raven_abi_fixture
_raven_abi_fixture:
  sub sp, sp, #96
  stp x29, x30, [sp, #64]
  str x18, [sp, #80]
  mov x16, x0
  mov x9, #9
  mov x10, #10
  stp x9, x10, [sp]
  mov x9, #11
  mov x10, #12
  stp x9, x10, [sp, #16]
  mov x9, #13
  mov x10, #14
  stp x9, x10, [sp, #32]
  mov x9, #15
  mov x10, #16
  stp x9, x10, [sp, #48]
  mov x0, #1
  mov x1, #2
  mov x2, #3
  mov x3, #4
  mov x4, #5
  mov x5, #6
  mov x6, #7
  mov x7, #8
  blr x16
  mov x23, #0x123
  mov x24, #0x456
  mov x28, #0x789
  ldr x18, [sp, #80]
  ldp x29, x30, [sp, #64]
  add sp, sp, #96
  ret
