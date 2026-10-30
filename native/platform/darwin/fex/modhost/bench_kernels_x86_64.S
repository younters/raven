// SPDX-License-Identifier: MIT
// Position-independent byte-identical kernels for Rosetta and a future FEX driver.
// SysV ABI: integer(count=rdi, seed=rsi); float(count=rdi, x=xmm0, a=xmm1, b=xmm2).
.text
.p2align 4
.globl _spike_integer, _spike_integer_end
_spike_integer:
	movq %rsi, %rax
	testq %rdi, %rdi
	jz 2f
1:  imulq $1664525, %rax, %rax
	addq $1013904223, %rax
	decq %rdi
	jnz 1b
2:  ret
_spike_integer_end:
.p2align 4
.globl _spike_float, _spike_float_end
_spike_float:
	testq %rdi, %rdi
	jz 2f
1:  mulsd %xmm1, %xmm0
	addsd %xmm2, %xmm0
	decq %rdi
	jnz 1b
2:  ret
_spike_float_end:
