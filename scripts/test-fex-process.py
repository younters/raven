#!/usr/bin/env python3
"""Run one real Raven process fixture; missing modules skip, incorrect exits fail."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from fex_modhost_contract import verify_patch_pins, verify_module_pin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--raven', type=Path, required=True)
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--modules', type=Path)
parser.add_argument('--backend', choices=('fex', 'native'), default='fex')
parser.add_argument('--timeout-ms', type=int, default=15000)
parser.add_argument('--expect-placement-conflict', action='store_true')
args = parser.parse_args()
root = args.modules or (Path(os.environ['RAVEN_FEX_MODULE_ROOT']) if os.environ.get('RAVEN_FEX_MODULE_ROOT') else None)
if args.backend == 'fex':
    if root is None or not (root / 'xtajit64.dll').is_file():
        print('SKIP: provision RAVEN_FEX_MODULE_ROOT/xtajit64.dll', flush=True)
        sys.exit(77)
    verify_module_pin(root / 'xtajit64.dll', verify_patch_pins())
with tempfile.TemporaryDirectory(prefix='raven-fex-process-') as prefix:
    env = dict(os.environ, RAVEN_PREFIX=prefix)
    if root is not None:
        env['RAVEN_FEX_MODULE_ROOT'] = str(root)
    command = [str(args.raven), '--run-process', '--execution=' + args.backend,
               '--timeout-ms=' + str(args.timeout_ms), str(args.fixture)]
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True,
                                timeout=args.timeout_ms / 1000 + 10)
    except subprocess.TimeoutExpired as error:
        print(f'FAIL: process exceeded outer timeout: {error}', file=sys.stderr)
        sys.exit(1)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if args.expect_placement_conflict:
        if result.returncode and 'PE preferred base conflicts with host mappings and image has no relocation table' in result.stderr:
            print('PASS: non-relocatable low image rejected without replacing host mappings')
            sys.exit(0)
        print('FAIL: expected a specific non-relocatable image conflict', file=sys.stderr)
        sys.exit(1)
    if result.returncode or 'Native entry returned 0 ' not in result.stdout:
        print(f'FAIL: expected verified zero entry return, host exit={result.returncode}', file=sys.stderr)
        sys.exit(1)
