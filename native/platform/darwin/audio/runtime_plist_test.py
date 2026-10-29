"""Inspect the actual runtime's embedded microphone usage description."""
import plistlib
import subprocess
import sys

text = subprocess.check_output(['otool', '-s', '__TEXT', '__info_plist', sys.argv[1]], text=True)
hex_data = ''.join(''.join(line.split()[1:]) for line in text.splitlines()[2:])
plist = plistlib.loads(bytes.fromhex(hex_data).rstrip(b'\0'))
assert plist.get('NSMicrophoneUsageDescription'), plist
print('Runtime embedded microphone usage description verified')
