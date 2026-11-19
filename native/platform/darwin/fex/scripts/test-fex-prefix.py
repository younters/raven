#!/usr/bin/env python3
"""Require real fresh-prefix COM registration under the inherited FEX engine."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
raven, fixture, modules, engine, width = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='raven-integ-registration-') as directory:
    env = dict(os.environ, RAVEN_PREFIX=directory, RAVEN_FEX_MODULE_ROOT=modules)
    env.pop('RAVEN_PREFIX_SETUP', None)
    env.pop('RAVEN_PARALLEL', None)
    start = time.perf_counter()
    try:
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('test-fex-process.py')),
            '--require-modules', '--backend', engine, '--raven', raven, '--fixture', fixture, '--modules', modules,
            '--prefix', directory, '--timeout-ms', '90000'], env=env, text=True, capture_output=True, timeout=105)
    except subprocess.TimeoutExpired:
        sys.exit('FAIL: fresh-prefix registration exceeded 105s (CTest budget: 120s)')
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    marker = Path(directory) / 'drive_c/windows/raven-registered.txt'
    servers = ('msxml3.dll', 'windowscodecs.dll', 'mlang.dll', 'propsys.dll')
    recorded = marker.read_text() if marker.exists() else ''
    paths = [line.split(' ', 1)[0] for line in recorded.splitlines()]
    complete = all(any(Path(path).name == server and ('/x86/' in path) == x86
                       for path in paths) for server in servers for x86 in (False, True))
    # The fixture checks that its loaded Windows codec comes from this path.
    system = 'syswow64' if width == '32' else 'system32'
    if not (Path(directory) / 'drive_c/windows' / system / 'regsvr32.exe').exists():
        sys.exit(f'FAIL: missing selected-width {system}/regsvr32.exe')
    if result.returncode or not complete:
        sys.exit('FAIL: fresh FEX prefix did not complete COM registration')
    print(f'PASS: fresh prefix engine={engine} width={width} registration_ms={(time.perf_counter()-start)*1000:.1f}; windowscodecs.dll and every setup server registered')
