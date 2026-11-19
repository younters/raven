#!/usr/bin/env python3
"""Check the public FEX launch path, discovery, override and SDK isolation."""
import os
from pathlib import Path
import re
import subprocess
import sys

raven, fixture = map(Path, sys.argv[1:3])
modules = Path(sys.argv[3]) if sys.argv[3] else None
host = raven.with_name('raven-fex-host')
def sdk(binary):
    output = subprocess.check_output(['otool', '-l', str(binary)], text=True)
    match = re.search(r'cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform \d+\s+minos ([\d.]+)\s+sdk ([\d.]+)', output)
    assert match, f'{binary}: missing LC_BUILD_VERSION in otool output'
    return match.groups()
assert sdk(host) == ('11.0', '11.0'), sdk(host)
assert sdk(raven)[1] != '11.0', sdk(raven)
env = dict(os.environ)
env.pop('RAVEN_FEX_MODULE_ROOT', None)
env['RAVEN_FEX_MODULE_ROOT'] = '/nonexistent/raven-fex-override'
command = [str(raven), '--run-process', '--execution=fex', '--timeout-ms=10000', str(fixture)]
r = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
assert r.returncode != 0 and 'FEX module override missing xtajit64.dll' in r.stdout + r.stderr, r
if modules is None or not (modules / 'xtajit64.dll').is_file():
    print('SKIP: configured module root is empty or missing xtajit64.dll; provision modules for default discovery execution')
    sys.exit(77)
env.pop('RAVEN_FEX_MODULE_ROOT')
r = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
assert r.returncode == 0 and 'Native entry returned 0 ' in r.stdout + r.stderr, r
env['RAVEN_FEX_MODULE_ROOT'] = ''
r = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
assert r.returncode == 0 and 'Native entry returned 0 ' in r.stdout + r.stderr, r
print('Default FEX discovery, override and SDK isolation passed')
