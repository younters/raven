"""Inspect the actual runtime's embedded microphone usage description."""
from pathlib import Path
import plistlib
import struct
import sys

binary = Path(sys.argv[1]).read_bytes()
assert struct.unpack_from('<I', binary)[0] == 0xfeedfacf, 'expected arm64 Mach-O'
at = 32
plist = None
for _ in range(struct.unpack_from('<I', binary, 16)[0]):
    command, length = struct.unpack_from('<II', binary, at)
    assert length >= 8 and at + length <= len(binary)
    if command == 0x19:  # LC_SEGMENT_64
        count = struct.unpack_from('<I', binary, at + 64)[0]
        for index in range(count):
            section = at + 72 + index * 80
            if binary[section:section+16].rstrip(b'\0') == b'__info_plist':
                size, offset = struct.unpack_from('<QI', binary, section + 40)
                plist = plistlib.loads(binary[offset:offset+size].rstrip(b'\0'))
    at += length
assert plist and plist.get('NSMicrophoneUsageDescription'), plist
print('Runtime embedded microphone usage description verified')
