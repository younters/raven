#!/usr/bin/env python3
"""Check a native fault rejection in an isolated, provisioned engine process."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import sys
from fex_modhost_contract import verify_patch_pins, verify_module_pin, verify_wow64_module_pin
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', required=True)
p.add_argument('--fixture', required=True)
p.add_argument('--modules')
p.add_argument('--wow64', action='store_true')
p.add_argument('--mode', default='internal')
p.add_argument('--expected-exit', type=int, default=133)
a = p.parse_args()
value = a.modules or os.environ.get('RAVEN_FEX_MODULE_ROOT')
root = Path(value) if value else None
module = 'xtajit.dll' if a.wow64 else 'xtajit64.dll'
if root is None or not (root / module).is_file():
    print(f'{"FAIL" if a.wow64 else "SKIP"}: provision RAVEN_FEX_MODULE_ROOT/{module}')
    sys.exit(1 if a.wow64 else 77)
(verify_wow64_module_pin if a.wow64 else verify_module_pin)(root / module, verify_patch_pins())
r = subprocess.run([a.host, a.fixture, a.mode], env=dict(os.environ, RAVEN_FEX_MODULE_ROOT=str(root)),
                   capture_output=True, text=True, timeout=10)
print(r.stdout, end='')
print(r.stderr, end='', file=sys.stderr)
diagnostic = (rf'FEX native signal={a.expected_exit - 128} code=-?\d+ esr=[0-9a-f]+ '
              r'pc=[0-9a-f]+ address=[0-9a-f]+ entry=(?:0x[0-9a-f]+|\(nil\)) jit=[01]\n'
              r'unhandled FEX native or unsupported fault\n')
if r.returncode != a.expected_exit or not re.fullmatch(diagnostic, r.stderr):
    sys.exit(f'FAIL: expected native fault rejection exit {a.expected_exit}, got {r.returncode}')
print(f'PASS: native fault rejected with exit {a.expected_exit}')
