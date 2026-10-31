// SPDX-License-Identifier: MIT
.text
.globl _raven_entry
_raven_entry:
	// No stack realignment: an ABI-conforming caller supplies ESP == 12 (mod 16).
	subl $28, %esp
	movl %esp, %eax
	andl $15, %eax
	jnz 1f
	movaps %xmm0, (%esp)
	movaps (%esp), %xmm0
	movl $500500, %eax
	addl $28, %esp
	ret
1:
	ud2
