#!/usr/bin/env python3
"""Measure verified Raven process fixtures with alternating native/FEX order."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time
from fex_modhost_contract import verify_module_pin, verify_patch_pins

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True, help='CMake build root')
parser.add_argument('--modules', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--compute', type=Path, help='Optional verified process compute PE')
parser.add_argument('--parallel', choices=('off', 'on'), default='off')
parser.add_argument('--samples', type=int, default=6)
args = parser.parse_args()
if args.samples < 1:
    parser.error('collect at least one sample')
try:
    verify_module_pin(args.modules / 'xtajit64.dll', verify_patch_pins())
except (RuntimeError, OSError) as error:
    parser.error(str(error))
raven = args.build / 'native/raven'
repo = Path(__file__).resolve().parents[1]
if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain']):
    parser.error('benchmark requires a clean source tree')
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
result = dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              platform=platform.platform(), machine=platform.machine(),
              parallel=args.parallel,
              scope='whole process wall time; isolated fresh prefix per launch; one warmup per backend excluded; balanced ABBA order',
              source_head=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              raven_sha256=sha(raven), module_sha256=sha(args.modules / 'xtajit64.dll'),
              benchmark_script_sha256=sha(Path(__file__)),
              source_diff_sha256=hashlib.sha256(subprocess.check_output(['git', '-C', str(repo), 'diff', 'HEAD', '--'])).hexdigest(), fixtures={})
fixtures = {name: args.build / 'native' / (name + '-fex.exe') for name in
            ('kernel-ansi', 'kernel-file', 'kernel-threads', 'kernel-seh')}
fixtures['compute'] = args.compute or args.build / 'native/darwin-parallel-benchmark.exe'
for name, fixture in fixtures.items():
    record = dict(fixture=str(fixture), sha256=sha(fixture), native=[], fex=[])
    result['fixtures'][name] = record
    for sample in range(-1, args.samples):
        for backend in (('native', 'fex') if sample % 2 == 0 else ('fex', 'native')):
            command = [str(raven), '--run-process', '--execution=' + backend,
                       '--timeout-ms=15000', str(fixture)]
            with tempfile.TemporaryDirectory(prefix='raven-m2-bench-') as prefix:
                env = dict(os.environ, RAVEN_PREFIX=prefix, RAVEN_FEX_MODULE_ROOT=str(args.modules))
                for key in ('RAVEN_TRACE_API', 'RAVEN_TRACE_FAULT', 'RAVEN_FEX_TRACE', 'RAVEN_FEX_TRACE_MEMORY', 'RAVEN_PARALLEL'):
                    env.pop(key, None)
                if args.parallel == 'on':
                    env['RAVEN_PARALLEL'] = '1'
                start = time.perf_counter_ns()
                proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=25)
                elapsed = (time.perf_counter_ns() - start) / 1e6
            item = dict(index=sample, command=command, returncode=proc.returncode,
                        stdout=proc.stdout, stderr=proc.stderr, wall_ms=elapsed)
            if sample >= 0:
                record[backend].append(item)
            if proc.returncode or 'Native entry returned 0 ' not in proc.stdout:
                args.output.write_text(json.dumps(result, indent=2) + '\n')
                raise RuntimeError(f'{name}/{backend} did not complete correctly: {item}')
    record['median_ms'] = {b: statistics.median(x['wall_ms'] for x in record[b]) for b in ('native', 'fex')}
    record['range_ms'] = {b: [min(x['wall_ms'] for x in record[b]), max(x['wall_ms'] for x in record[b])] for b in ('native', 'fex')}
    print(name, record['median_ms'], flush=True)
args.output.write_text(json.dumps(result, indent=2) + '\n')
