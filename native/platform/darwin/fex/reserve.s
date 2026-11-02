// SPDX-License-Identifier: MIT
// Pre-dyld ownership for conventional x64 EXE bases. Keep the slid segment
// below the macOS 26 shared cache at 0x180000000. DLLs outside the pool use
// the normal non-overwriting reservation/relocation path.
.globl _raven_pe_reserve
.globl _raven_pe_reserve_end
.zerofill RAVEN_PE,RAVEN_PE,_raven_pe_reserve,0x40000000
.zerofill RAVEN_PE,RAVEN_PE,_raven_pe_reserve_end,0
