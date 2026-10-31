// SPDX-License-Identifier: MIT
.text
.p2align 2
.globl _raven_capture_ec_context
_raven_capture_ec_context:
	mov x13, x0
	mov x14, #0
1:
	str xzr, [x13, x14]
	add x14, x14, #8
	cmp x14, #0x4d0
	b.lo 1b
	str x8, [x13, #120]
	str x0, [x13, #128]
	str x1, [x13, #136]
	str x27, [x13, #144]
	str x29, [x13, #160]
	str x25, [x13, #168]
	str x26, [x13, #176]
	str x2, [x13, #184]
	str x3, [x13, #192]
	str x4, [x13, #200]
	str x5, [x13, #208]
	str x19, [x13, #216]
	str x20, [x13, #224]
	str x21, [x13, #232]
	str x22, [x13, #240]
	str x30, [x13, #288]
	str x6, [x13, #304]
	str x7, [x13, #320]
	str x9, [x13, #336]
	str x10, [x13, #352]
	str x11, [x13, #368]
	str x12, [x13, #384]
	str x15, [x13, #400]
	str x30, [x13, #0xf8]
	mov x14, sp
	str x14, [x13, #0x98]
	mov w14, #0xb
	movk w14, #1, lsl #16
	str w14, [x13, #0x30]
	mov w14, #0x202
	str w14, [x13, #0x44]
	mov w14, #0x27f
	strh w14, [x13, #0x100]
	mov w14, #0x1f80
	str w14, [x13, #0x118]
	str w14, [x13, #0x34]
	str q0, [x13, #416]
	str q1, [x13, #432]
	str q2, [x13, #448]
	str q3, [x13, #464]
	str q4, [x13, #480]
	str q5, [x13, #496]
	str q6, [x13, #512]
	str q7, [x13, #528]
	str q8, [x13, #544]
	str q9, [x13, #560]
	str q10, [x13, #576]
	str q11, [x13, #592]
	str q12, [x13, #608]
	str q13, [x13, #624]
	str q14, [x13, #640]
	str q15, [x13, #656]
	ret
.p2align 2
.globl _raven_restore_ec_context
_raven_restore_ec_context:
	mov x13, x0
	ldr x8, [x13, #120]
	ldr x1, [x13, #136]
	ldr x27, [x13, #144]
	ldr x29, [x13, #160]
	ldr x25, [x13, #168]
	ldr x26, [x13, #176]
	ldr x2, [x13, #184]
	ldr x3, [x13, #192]
	ldr x4, [x13, #200]
	ldr x5, [x13, #208]
	ldr x19, [x13, #216]
	ldr x20, [x13, #224]
	ldr x21, [x13, #232]
	ldr x22, [x13, #240]
	ldr x30, [x13, #288]
	ldr x6, [x13, #304]
	ldr x7, [x13, #320]
	ldr x9, [x13, #336]
	ldr x10, [x13, #352]
	ldr x11, [x13, #368]
	ldr x12, [x13, #384]
	ldr x15, [x13, #400]
	ldr q0, [x13, #416]
	ldr q1, [x13, #432]
	ldr q2, [x13, #448]
	ldr q3, [x13, #464]
	ldr q4, [x13, #480]
	ldr q5, [x13, #496]
	ldr q6, [x13, #512]
	ldr q7, [x13, #528]
	ldr q8, [x13, #544]
	ldr q9, [x13, #560]
	ldr q10, [x13, #576]
	ldr q11, [x13, #592]
	ldr q12, [x13, #608]
	ldr q13, [x13, #624]
	ldr q14, [x13, #640]
	ldr q15, [x13, #656]
	ldr x14, [x13, #0x98]
	mov sp, x14
	ldr x16, [x13, #0xf8]
	ldr x0, [x13, #0x80]
	br x16

// Windows ARM64 context capture for the native WoW64 frontend. This entry is
// called directly from Windows code, before any Darwin import adapter frame.
.p2align 2
.globl _raven_capture_arm64_context
_raven_capture_arm64_context:
	stp x0, x1, [x0, #8]
	stp x2, x3, [x0, #24]
	stp x4, x5, [x0, #40]
	stp x6, x7, [x0, #56]
	stp x8, x9, [x0, #72]
	stp x10, x11, [x0, #88]
	stp x12, x13, [x0, #104]
	stp x14, x15, [x0, #120]
	stp x16, x17, [x0, #136]
	stp x18, x19, [x0, #152]
	stp x20, x21, [x0, #168]
	stp x22, x23, [x0, #184]
	stp x24, x25, [x0, #200]
	stp x26, x27, [x0, #216]
	stp x28, x29, [x0, #232]
	str x30, [x0, #248]
	mov x16, sp
	str x16, [x0, #256]
	str x30, [x0, #264]
	mov w16, #7
	movk w16, #0x40, lsl #16
	str w16, [x0]
	mrs x16, nzcv
	str w16, [x0, #4]
	stp q0, q1, [x0, #272]
	stp q2, q3, [x0, #304]
	stp q4, q5, [x0, #336]
	stp q6, q7, [x0, #368]
	stp q8, q9, [x0, #400]
	stp q10, q11, [x0, #432]
	stp q12, q13, [x0, #464]
	stp q14, q15, [x0, #496]
	stp q16, q17, [x0, #528]
	stp q18, q19, [x0, #560]
	stp q20, q21, [x0, #592]
	stp q22, q23, [x0, #624]
	stp q24, q25, [x0, #656]
	stp q26, q27, [x0, #688]
	stp q28, q29, [x0, #720]
	stp q30, q31, [x0, #752]
	mrs x16, fpcr
	str w16, [x0, #784]
	mrs x16, fpsr
	str w16, [x0, #788]
	ldp x16, x17, [x0, #136]
	ret

// ARM64 Windows __chkstk takes the frame size in 16-byte units in x15.
// Probe before the caller adjusts SP, preserving all ordinary argument registers.
.p2align 2
.globl _raven_arm64_chkstk
_raven_arm64_chkstk:
	lsl x16, x15, #4
	mov x17, sp
1:
	cmp x16, #4096
	b.lo 2f
	sub x17, x17, #4096
	ldr xzr, [x17]
	sub x16, x16, #4096
	b 1b
2:
	sub x17, x17, x16
	ldr xzr, [x17]
	ret
