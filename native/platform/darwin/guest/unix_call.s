// Win64 -> Darwin SysV on the calling Windows pthread. Native callbacks into PE are unsupported.
// Keep the whole transition in this range so an interrupt cannot give its stack or TSD to another Windows thread.
.text
.p2align 4
.globl _raven_unix_call_gate
.globl _raven_unix_call_gate_end
.globl _raven_unix_call_native_begin
.globl _raven_unix_call_native_end
_raven_unix_call_gate:
	// SYS_thread_selfid has no arguments and needs neither GS nor libc. Preserve the Win64 handle across syscall.
	movq %rcx, %r9
	movq %rdx, %r10
	movl $0x2000174, %eax
	syscall
	movq %r10, %rdx
	leaq _raven_unix_threads(%rip), %r10
	movl $256, %r11d
1:
	cmpq (%r10), %rax
	je 2f
	addq $64, %r10
	decl %r11d
	jnz 1b
	movl $0xc0000008, %eax
	retq
2:
	movq %r9, %rcx
	movq 32(%r10), %rax
	movl $1, (%rax)
	movq %rsp, %rax
	movq 8(%r10), %rsp
	pushq %rax
	pushq %rbx
	pushq %rbp
	pushq %rsi
	pushq %rdi
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq $568, %rsp
	fxsave64 (%rsp)
	movq %rcx, 512(%rsp)
	movl %edx, 520(%rsp)
	movq %r8, 528(%rsp)
	movq %r10, 552(%rsp)
	movq 24(%r10), %rax
	movq (%rax), %rax
	movq %rax, 536(%rsp)
	movq 16(%r10), %rdi
	movl $0x3000003, %eax
	syscall
_raven_unix_call_native_begin:
	cld
	movq 512(%rsp), %rdi
	movl 520(%rsp), %esi
	movq 528(%rsp), %rdx
	callq _raven_dispatch_unix_call
	movq 552(%rsp), %r10
	movq 32(%r10), %r10
	movl $0, (%r10)
	movl %eax, 544(%rsp)
	fxrstor64 (%rsp)
	movq 536(%rsp), %rdi
	movl $0x3000003, %eax
	syscall
_raven_unix_call_native_end:
	movl 544(%rsp), %eax
	addq $568, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rdi
	popq %rsi
	popq %rbp
	popq %rbx
	popq %rsp
	retq
_raven_unix_call_gate_end:
