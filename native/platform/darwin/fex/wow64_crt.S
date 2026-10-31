// SPDX-License-Identifier: MIT
.data
source: .asciz "ntdll cdecl"
destination: .space 16
.text
.globl _raven_entry
_raven_entry:
	pushl %ebx
	movl %esp, %ebx
	pushl $12
	pushl $source
	pushl $destination
	call _memcpy
	addl $12, %esp
	cmpl %ebx, %esp
	jne 1f
	cmpl $destination, %eax
	jne 1f
	cmpl $0x6c64746e, destination
	jne 1f
	xorl %eax, %eax
	popl %ebx
	ret
1:
	movl %ebx, %esp
	movl $1, %eax
	popl %ebx
	ret
