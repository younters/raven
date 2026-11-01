#!/usr/bin/env python3
"""Offline Raven JS smoke benchmark, not a Speedometer/JetStream score.

Use an APFS clone of a Chrome prefix. Each launch uses a separate Chrome profile.
Only process groups created by this script are terminated. stdout contains the
result from --dump-dom; missing or incorrect results fail, never become timings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--raven', type=Path, required=True)
    p.add_argument('--prefix', type=Path, required=True, help='Disposable cloned Chrome prefix')
    p.add_argument('--modules', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--samples', type=int, default=1, choices=range(1, 4))
    p.add_argument('--timeout', type=float, default=120)
    a = p.parse_args()
    page = Path(__file__).resolve().parent / 'benchmarks/chrome-js.html'
    chrome = a.prefix.resolve() / 'drive_c/Program Files/Google/Chrome/Application/chrome.exe'
    result = {'scope': 'offline synthetic JS; headless; parallel; GPU enabled; milliseconds lower is better',
              'raven_sha256': hashlib.sha256(a.raven.read_bytes()).hexdigest(),
              'module_sha256': hashlib.sha256((a.modules / 'xtajit64.dll').read_bytes()).hexdigest(),
              'page_sha256': hashlib.sha256(page.read_bytes()).hexdigest(), 'samples': []}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    for sample in range(a.samples):
        for engine in (('native', 'fex') if sample % 2 == 0 else ('fex', 'native')):
            profile = a.prefix / f'bench-profile-{engine}-{time.time_ns()}'
            command = [str(a.raven.resolve()), '--run-process', '--execution=' + engine,
                       '--timeout-ms=' + str(int(a.timeout * 1000)), '--desktop=macos', '--stdio=inherit', str(chrome), '--',
                       '--headless=new', '--no-sandbox', '--no-first-run', '--no-default-browser-check',
                       '--disable-background-networking', '--user-data-dir=Z:' + str(profile.resolve()),
                       '--dump-dom', 'file:///Z:' + str(page)]
            env = dict(os.environ, RAVEN_PREFIX=str(a.prefix.resolve()), RAVEN_PARALLEL='1',
                       RAVEN_FEX_MODULE_ROOT=str(a.modules.resolve()))
            env.pop('RAVEN_PREFIX_SETUP', None)
            log = a.output.with_suffix(f'.{engine}-{sample}.log')
            item = {'engine': engine, 'sample': sample, 'command': command, 'log': str(log)}
            start = time.monotonic()
            with log.open('w') as stream:
                proc = subprocess.Popen(command, env=env, stdout=stream, stderr=stream, start_new_session=True)
                try:
                    proc.wait(timeout=a.timeout + 5)
                except subprocess.TimeoutExpired:
                    item['timeout'] = True
                finally:
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
            item.update(wall_seconds=time.monotonic()-start, returncode=proc.returncode)
            matches = re.findall(r'RAVEN_BENCH_RESULT=(\{[^\n<]*\})', log.read_text(errors='replace'))
            if matches:
                measurement = json.loads(matches[-1])
                if (measurement.get('primes'), measurement.get('length'), measurement.get('items')) == (216816, 200000, 400000):
                    item['result'] = measurement
            result['samples'].append(item)
            a.output.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(item), flush=True)
    return 0 if all('result' in s for s in result['samples']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
