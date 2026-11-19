// Linux x86-64 register layouts. The runtime keeps guest contexts in them on every host; the macOS controller fills them from the
// x86 thread state the guest agent reports.
#pragma once

struct user_regs_struct {
	unsigned long long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss,
		fs_base, gs_base, ds, es, fs, gs;
};

struct user_fpregs_struct {
	unsigned short cwd, swd, ftw, fop;
	unsigned long long rip, rdp;
	unsigned int mxcsr, mxcr_mask;
	unsigned int st_space[32];
	unsigned int xmm_space[64];
	unsigned int padding[24];
};
