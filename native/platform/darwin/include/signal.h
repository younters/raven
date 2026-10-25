#pragma once
#include_next <signal.h>

// si_code values the runtime reads from guest stops that macOS does not define. The macOS controller reports breakpoint and debug stops
// with these, as the kernel does on Linux; other codes are macOS's own (its FPE_* numbering differs from Linux's and is used as is).
#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 4
#endif
