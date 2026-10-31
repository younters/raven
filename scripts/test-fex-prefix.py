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
    result = subprocess.run([sys.executable, str(Path(__file__).with_name('test-fex-process.py')),
        '--require-modules', '--backend', engine, '--raven', raven, '--fixture', fixture, '--modules', modules,
        '--prefix', directory, '--timeout-ms', '90000'], env=env, text=True, capture_output=True, timeout=110)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    marker = Path(directory) / 'drive_c/windows/raven-registered.txt'
    if result.returncode or not marker.exists() or 'windowscodecs.dll' not in marker.read_text():
        sys.exit('FAIL: fresh FEX prefix did not complete COM registration')
    print(f'PASS: fresh prefix engine={engine} width={width} registration_ms={(time.perf_counter()-start)*1000:.1f}; windowscodecs.dll and every setup server registered')
