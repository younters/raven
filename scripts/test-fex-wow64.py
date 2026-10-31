#!/usr/bin/env python3
"""Require an actual checked PE32 entry/process return from Raven's FEX backend."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from fex_modhost_contract import verify_patch_pins, verify_wow64_module_pin
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--raven', required=True)
parser.add_argument('--fixture', required=True)
parser.add_argument('--modules', required=True)
parser.add_argument('--entry', action='store_true')
parser.add_argument('--parallel', action='store_true')
args = parser.parse_args()
root = Path(args.modules)
if not args.modules or not (root / 'xtajit.dll').is_file():
    print('SKIP: provision RAVEN_FEX_MODULE_ROOT/xtajit.dll')
    sys.exit(77)
verify_wow64_module_pin(root / 'xtajit.dll', verify_patch_pins())
with tempfile.TemporaryDirectory(prefix='raven-m4-') as prefix:
    env = dict(os.environ, RAVEN_FEX_MODULE_ROOT=str(root), RAVEN_PREFIX=prefix)
    env.pop('RAVEN_PARALLEL', None)
    if args.parallel:
        env['RAVEN_PARALLEL'] = '1'
        env['RAVEN_SLICE_MS'] = '0'
    cmd = [args.raven, '--run-entry' if args.entry else '--run-process', '--execution=fex', args.fixture]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=25)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    expected = 500500 if args.entry else 0
    if result.returncode or f'Native entry returned {expected} ' not in result.stdout:
        sys.exit(f'FAIL: expected checked PE32 return {expected}, host={result.returncode}')
