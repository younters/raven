#!/usr/bin/env python3
"""Run one real Raven process fixture; missing modules skip, incorrect exits fail."""
import argparse
import contextlib
import os
from pathlib import Path
import subprocess
import sys
import struct
import tempfile
from fex_modhost_contract import verify_patch_pins, verify_module_pin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--require-modules', action='store_true')
parser.add_argument('--raven', type=Path, required=True)
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--modules')
parser.add_argument('--backend', choices=('fex', 'native'), default='fex')
parser.add_argument('--timeout-ms', type=int, default=15000)
parser.add_argument('--expect-placement-conflict', action='store_true')
parser.add_argument('--prefix', type=Path, help='Retain a shared fixture prefix instead of creating a temporary one')
parser.add_argument('--check-persistence', action='store_true')
parser.add_argument('--unhandled-status', type=lambda value: int(value, 0))
args = parser.parse_args()
module_value = args.modules or os.environ.get('RAVEN_FEX_MODULE_ROOT')
root = Path(module_value) if module_value else None
if args.backend == 'fex':
    if root is None or not (root / 'xtajit64.dll').is_file():
        print(('FAIL' if args.require_modules else 'SKIP') + ': provision RAVEN_FEX_MODULE_ROOT/xtajit64.dll', flush=True)
        sys.exit(1 if args.require_modules else 77)
    try:
        verify_module_pin(root / 'xtajit64.dll', verify_patch_pins())
    except (RuntimeError, OSError) as error:
        parser.error(str(error))
if args.check_persistence:
    if args.prefix is None:
        parser.error('--check-persistence requires --prefix')
    sys.exit(subprocess.call([sys.executable, str(Path(__file__).with_name('check-file-persistence.py')), str(args.prefix)]))
with contextlib.ExitStack() as stack:
    prefix = args.prefix or Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='raven-fex-process-')))
    env = dict(os.environ, RAVEN_PREFIX=str(prefix))
    if root is not None:
        env['RAVEN_FEX_MODULE_ROOT'] = str(root)
    command = [str(args.raven), '--run-process', '--execution=' + args.backend,
                '--timeout-ms=' + str(args.timeout_ms)]
    descriptors = ()
    if args.unhandled_status is not None:
        exit_file = stack.enter_context(tempfile.TemporaryFile())
        descriptors = (exit_file.fileno(),)
        command.append('--exit-code-fd=' + str(exit_file.fileno()))
    command.append(str(args.fixture))
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True,
                                timeout=args.timeout_ms / 1000 + 10, pass_fds=descriptors)
    except subprocess.TimeoutExpired as error:
        print(f'FAIL: process exceeded outer timeout: {error}', file=sys.stderr)
        sys.exit(1)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if args.expect_placement_conflict:
        if result.returncode and 'PE preferred base is unavailable and image has no relocation table' in result.stderr:
            print('PASS: unavailable fixed image rejected without replacing existing mappings')
            sys.exit(0)
        print('FAIL: expected a specific non-relocatable image conflict', file=sys.stderr)
        sys.exit(1)
    if args.unhandled_status is not None:
        status = args.unhandled_status
        exit_file.seek(0)
        recorded = exit_file.read()
        expected_host = (status & 0xff) or (1 if status else 0)
        if (result.returncode != expected_host or len(recorded) != 4 or struct.unpack('<I', recorded)[0] != status or
                f'Native process exited with status {status}' not in result.stdout or
                f'unhandled exception {status:08x}' not in result.stderr):
            print(f'FAIL: unhandled exception did not preserve NT status {status:#x}; host={result.returncode}, channel={recorded.hex()}', file=sys.stderr)
            sys.exit(1)
        print(f'PASS: unhandled NT status {status:#x} preserved in process exit channel')
        sys.exit(0)
    if result.returncode or 'Native entry returned 0 ' not in result.stdout:
        print(f'FAIL: expected verified zero entry return, host exit={result.returncode}', file=sys.stderr)
        sys.exit(1)
