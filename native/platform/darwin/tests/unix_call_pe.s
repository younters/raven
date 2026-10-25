.text
.globl check_preserved
check_preserved:
	pushq %rbx
	pushq %rbp
	pushq %rdi
	pushq %rsi
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq $248, %rsp
	movq %rcx, 200(%rsp)
	movq %rdx, 208(%rsp)
	movq %r8, 192(%rsp)
.macro save_vector reg, offset
	movdqu %\reg, \offset(%rsp)
	pcmpeqd %\reg, %\reg
.endm
	save_vector xmm6, 32
	save_vector xmm7, 48
	save_vector xmm8, 64
	save_vector xmm9, 80
	save_vector xmm10, 96
	save_vector xmm11, 112
	save_vector xmm12, 128
	save_vector xmm13, 144
	save_vector xmm14, 160
	save_vector xmm15, 176
.macro seed reg, value
	movq $\value, %\reg
.endm
	seed rbx, 0x123
	seed rbp, 0x234
	seed rdi, 0x345
	seed rsi, 0x456
	seed r12, 0x567
	seed r13, 0x678
	seed r14, 0x789
	seed r15, 0x89a
	movq 208(%rsp), %rcx
	xorl %edx, %edx
	movq 192(%rsp), %r8
	callq *200(%rsp)
	cmpl $0x1234, %eax
	jne failed
.macro check_register reg, value
	cmpq $\value, %\reg
	jne failed
.endm
	check_register rbx, 0x123
	check_register rbp, 0x234
	check_register rdi, 0x345
	check_register rsi, 0x456
	check_register r12, 0x567
	check_register r13, 0x678
	check_register r14, 0x789
	check_register r15, 0x89a
.macro check_vector reg
	pmovmskb %\reg, %eax
	cmpl $0xffff, %eax
	jne failed
.endm
	check_vector xmm6
	check_vector xmm7
	check_vector xmm8
	check_vector xmm9
	check_vector xmm10
	check_vector xmm11
	check_vector xmm12
	check_vector xmm13
	check_vector xmm14
	check_vector xmm15
	movl $1, %eax
	jmp done
failed:
	xorl %eax, %eax
done:
	movdqu 32(%rsp), %xmm6
	movdqu 48(%rsp), %xmm7
	movdqu 64(%rsp), %xmm8
	movdqu 80(%rsp), %xmm9
	movdqu 96(%rsp), %xmm10
	movdqu 112(%rsp), %xmm11
	movdqu 128(%rsp), %xmm12
	movdqu 144(%rsp), %xmm13
	movdqu 160(%rsp), %xmm14
	movdqu 176(%rsp), %xmm15
	addq $248, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rsi
	popq %rdi
	popq %rbp
	popq %rbx
	retq
