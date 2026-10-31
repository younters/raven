// SPDX-License-Identifier: MIT
// Pre-dyld ownership for conventional x64 EXE/DLL bases. It slides with the
// arm64 host; launch.cpp transfers only the main image slice to the PE loader.
.globl _raven_pe_reserve
.zerofill RAVEN_PE,RAVEN_PE,_raven_pe_reserve,0x100000000
