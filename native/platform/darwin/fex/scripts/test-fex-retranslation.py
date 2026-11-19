#!/usr/bin/env python3
"""Check guest byte coherence and translation retention at 4 KiB granularity."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--raven', required=True, type=Path)
p.add_argument('--fixture', required=True, type=Path)
p.add_argument('--modules', required=True, type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='raven-retranslation-test-') as prefix:
    env = dict(os.environ, RAVEN_PREFIX=prefix, RAVEN_FEX_MODULE_ROOT=str(a.modules),
               RAVEN_FEX_PROFILE_COMPILE='1')
    result = subprocess.run([str(a.raven), '--run-process', '--execution=fex',
                             '--timeout-ms=15000', '--stdio=inherit', str(a.fixture)],
                            env=env, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    print(output)
    assert result.returncode == 0, f'guest exit {result.returncode}'
    marker = re.search(r'RETRANSLATION_CODE=([0-9a-f]{16})', output)
    assert marker, 'guest code address missing'
    page = int(marker[1], 16) & ~4095
    counters = re.search(rf'Raven translations page={page:x} unique=(\d+) repeated=(\d+) recompiled=(\d+)', output)
    assert counters, 'translation counters missing'
    assert tuple(map(int, counters.groups())) == (1, 2, 1), counters[0]
    print('PASS: neighbouring data and RX-only notifications retained code; changed bytes recompiled and real-self-handle flush invalidated unchanged code')
