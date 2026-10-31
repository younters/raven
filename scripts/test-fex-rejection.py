#!/usr/bin/env python3
"""Check a native fault rejection in an isolated, provisioned engine process."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from fex_modhost_contract import verify_patch_pins, verify_module_pin, verify_wow64_module_pin
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', required=True)
p.add_argument('--fixture', required=True)
p.add_argument('--modules')
p.add_argument('--wow64', action='store_true')
a = p.parse_args()
value = a.modules or os.environ.get('RAVEN_FEX_MODULE_ROOT')
root = Path(value) if value else None
module = 'xtajit.dll' if a.wow64 else 'xtajit64.dll'
if root is None or not (root / module).is_file():
    print(f'{"FAIL" if a.wow64 else "SKIP"}: provision RAVEN_FEX_MODULE_ROOT/{module}')
    sys.exit(1 if a.wow64 else 77)
(verify_wow64_module_pin if a.wow64 else verify_module_pin)(root / module, verify_patch_pins())
r = subprocess.run([a.host, a.fixture, 'internal'], env=dict(os.environ, RAVEN_FEX_MODULE_ROOT=str(root)),
                   capture_output=True, text=True, timeout=10)
print(r.stdout, end='')
print(r.stderr, end='', file=sys.stderr)
if r.returncode != 133 or r.stderr != 'unhandled FEX native or unsupported fault\n':
    sys.exit(f'FAIL: expected native BRK rejection exit 133, got {r.returncode}')
print('PASS: native BRK rejected with exit 133')
