#!/usr/bin/env python3
"""Verify the qualified host DLL actually sets CONFIG_MULTIBLOCK(0) to \"0\".

RVA evidence: llvm-objdump ProcessInit, private 0063 module. Changed code must
be requalified deliberately rather than accepting only source patch presence.
"""
import struct
import sys
from pathlib import Path

def verify(data):
    pe = struct.unpack_from('<I', data, 60)[0]
    count, optional = struct.unpack_from('<H', data, pe+6)[0], struct.unpack_from('<H', data, pe+20)[0]
    def read(rva, length):
        for i in range(count):
            at = pe+24+optional+i*40
            size, virtual, raw_size, raw = struct.unpack_from('<IIII', data, at+8)
            if virtual <= rva and rva+length <= virtual+raw_size:
                return data[raw+rva-virtual:raw+rva-virtual+length]
        raise RuntimeError('multiblock code RVA missing')
    # adrp x1, 0x180229000; add x1,x1,#0x96; mov w0,wzr;
    # mov w2,#1; bl FEXCore::Config::Set (option 0, string_view("0",1)).
    if read(0x6770,20) != bytes.fromhex('011100f021580291e0031f2a22008052c5180694') or read(0x229096,2)!=b'0\0':
        raise RuntimeError('qualified CONFIG_MULTIBLOCK=0 machine code missing; requalify module')

data=Path(sys.argv[1]).read_bytes()
verify(data)
# Negative control: replacing the only matching call must be rejected.
mutated=data.replace(bytes.fromhex('011100f021580291e0031f2a22008052c5180694'), b'\0'*20)
try:
    verify(mutated)
except RuntimeError:
    print('PASS: compiled multiblock workaround and negative control')
else:
    raise RuntimeError('negative control accepted')
