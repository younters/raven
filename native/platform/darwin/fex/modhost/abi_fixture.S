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
	movi v8.16b, #48
	movi v9.16b, #49
	movi v10.16b, #50
	movi v11.16b, #51
	movi v12.16b, #52
	movi v13.16b, #53
	movi v14.16b, #54
	movi v15.16b, #55
	mov x0, #1
	mov x1, #2
	mov x2, #3
	mov x3, #4
	mov x4, #5
	mov x5, #6
	mov x6, #7
	mov x7, #8
	blr x16
	ldr x9, [sp, #80]
	cmp x18, x9
	b.ne 9f
	movi v0.16b, #48
	cmeq v0.16b, v0.16b, v8.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #49
	cmeq v0.16b, v0.16b, v9.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #50
	cmeq v0.16b, v0.16b, v10.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #51
	cmeq v0.16b, v0.16b, v11.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #52
	cmeq v0.16b, v0.16b, v12.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #53
	cmeq v0.16b, v0.16b, v13.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #54
	cmeq v0.16b, v0.16b, v14.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	movi v0.16b, #55
	cmeq v0.16b, v0.16b, v15.16b
	uminv b0, v0.16b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 9f
	b 8f
9:
	mov x0, #0
8:
	mov x23, #0x123
	mov x24, #0x456
	mov x28, #0x789
	ldr x18, [sp, #80]
	ldp x29, x30, [sp, #64]
	add sp, sp, #96
	ret

.globl _raven_abi_native_clobber
_raven_abi_native_clobber:
	movi v6.16b, #0xee
	movi v7.16b, #0xee
	movi v0.16b, #0xee
	ins v8.d[1], v0.d[0]
	ins v9.d[1], v0.d[0]
	ins v10.d[1], v0.d[0]
	ins v11.d[1], v0.d[0]
	ins v12.d[1], v0.d[0]
	ins v13.d[1], v0.d[0]
	ins v14.d[1], v0.d[0]
	ins v15.d[1], v0.d[0]
	ret

.globl _raven_abi_verify_native
_raven_abi_verify_native:
	stp x29, x30, [sp, #-96]!
	stp d8, d9, [sp, #16]
	stp d10, d11, [sp, #32]
	stp d12, d13, [sp, #48]
	stp d14, d15, [sp, #64]
	str x18, [sp, #80]
	movi v8.8b, #32
	movi v9.8b, #33
	movi v10.8b, #34
	movi v11.8b, #35
	movi v12.8b, #36
	movi v13.8b, #37
	movi v14.8b, #38
	movi v15.8b, #39
	bl _raven_ec_call
	mov x12, x0
	movi v0.8b, #32
	cmeq v0.8b, v0.8b, v8.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #33
	cmeq v0.8b, v0.8b, v9.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #34
	cmeq v0.8b, v0.8b, v10.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #35
	cmeq v0.8b, v0.8b, v11.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #36
	cmeq v0.8b, v0.8b, v12.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #37
	cmeq v0.8b, v0.8b, v13.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #38
	cmeq v0.8b, v0.8b, v14.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	movi v0.8b, #39
	cmeq v0.8b, v0.8b, v15.8b
	uminv b0, v0.8b
	umov w9, v0.b[0]
	cmp w9, #255
	b.ne 7f
	ldr x9, [sp, #80]
	cmp x18, x9
	b.ne 7f
	b 6f
7:
	mov x12, #0
6:
	ldp d8, d9, [sp, #16]
	ldp d10, d11, [sp, #32]
	ldp d12, d13, [sp, #48]
	ldp d14, d15, [sp, #64]
	ldp x29, x30, [sp], #96
	mov x0, x12
	ret

.globl _raven_abi_spin
_raven_abi_spin:
	mov x9, x0
	mov x10, x18
5:
	cmp x18, x10
	b.ne 4f
	subs x9, x9, #1
	b.ne 5b
	mov x0, #1
	ret
4:
	mov x0, #0
	ret
