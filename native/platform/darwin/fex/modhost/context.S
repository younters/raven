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
