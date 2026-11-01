#!/usr/bin/env python3
"""Darwin Notepad++ first editor-window latency for identical native/FEX fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--raven', type=Path, required=True)
p.add_argument('--fixture', type=Path, required=True, help='Portable Notepad++ directory (copied per launch)')
p.add_argument('--modules', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--samples', type=int, default=6)
p.add_argument('--timeout', type=float, default=45)
a = p.parse_args()
if a.samples < 1:
    p.error('at least one sample required')
a.output.parent.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[1]
if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain']):
    p.error('benchmark requires a clean source tree')
result = dict(source_head=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(), scope='one excluded warmup per engine/mode; balanced ABBA; launch to process-owned Notepad++ window plus successfully created Scintilla child; 50ms polling',
              raven_sha256=hashlib.sha256(a.raven.read_bytes()).hexdigest(),
              module_sha256=hashlib.sha256((a.modules / 'xtajit64.dll').read_bytes()).hexdigest(), samples=[])
with tempfile.TemporaryDirectory(prefix='raven-m5-startup-') as temp:
    root = Path(temp)
    source = root / 'windows.swift'
    source.write_text('''import CoreGraphics
import Foundation
let pid = Int(CommandLine.arguments[1])!
let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String:Any]]
for w in windows where (w[kCGWindowOwnerPID as String] as? Int) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {
 if let name = w[kCGWindowName as String] as? String, name.contains("Notepad++") {
  print(w[kCGWindowNumber as String]!); break
 }
}
''')
    probe = root / 'windows'
    subprocess.run(['swiftc', str(source), '-o', str(probe)], check=True)
    for parallel in (False, True):
        for index in range(-1, a.samples):
            for backend in (('native', 'fex') if index % 2 == 0 else ('fex', 'native')):
                case = root / f'{parallel}-{index}-{backend}'
                subprocess.run(['cp', '-cR', str(a.fixture), str(case)], check=True)
                env = dict(os.environ, RAVEN_PREFIX=str(case / 'prefix'), RAVEN_FEX_MODULE_ROOT=str(a.modules), RAVEN_TRACE_WINDOW_CREATE='1')
                for key in ('RAVEN_TRACE_API', 'RAVEN_TRACE_FAULT', 'RAVEN_FEX_TRACE', 'RAVEN_PARALLEL'):
                    env.pop(key, None)
                if parallel:
                    env['RAVEN_PARALLEL'] = '1'
                command = [str(a.raven), '--run-process', '--execution=' + backend, '--timeout-ms=60000',
                           '--desktop=macos', str(case / 'notepad++.exe')]
                log = a.output.parent / f'npp-startup-{int(parallel)}-{backend}-{index}.log'
                item = dict(backend=backend, parallel=parallel, index=index, command=command, log=str(log))
                with log.open('w') as stream:
                    start = time.perf_counter()
                    proc = subprocess.Popen(command, env=env, stdout=stream, stderr=stream, start_new_session=True)
                    try:
                        while proc.poll() is None and time.perf_counter() - start < a.timeout:
                            window = subprocess.check_output([str(probe), str(proc.pid)], text=True).strip()
                            if window and 'class=Scintilla child=1' in log.read_text():
                                item['startup_ms'] = (time.perf_counter() - start) * 1000
                                # Capture after painting; this delay is outside the measured latency.
                                time.sleep(1)
                                shot = a.output.parent / f'npp-startup-{int(parallel)}-{backend}-{index}.png'
                                capture = subprocess.run(['/usr/sbin/screencapture', '-x', '-l', window, str(shot)])
                                if capture.returncode:
                                    item.pop('startup_ms', None)
                                    time.sleep(.05)
                                    continue
                                item['screenshot'] = str(shot)
                                break
                            time.sleep(.05)
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
                    item['returncode_after_cleanup'] = proc.returncode
                if index >= 0:
                    result['samples'].append(item)
                a.output.write_text(json.dumps(result, indent=2) + '\n')
                print(item, flush=True)
result['medians_ms'] = {}
result['ranges_ms'] = {}
for parallel in (False, True):
    for backend in ('native', 'fex'):
        items = [x for x in result['samples'] if x['parallel'] == parallel and x['backend'] == backend]
        result['ranges_ms'][f'{backend}/{int(parallel)}'] = [min(x['startup_ms'] for x in items), max(x['startup_ms'] for x in items)] if all('startup_ms' in x for x in items) else None
        result['medians_ms'][f'{backend}/{int(parallel)}'] = (statistics.median(x['startup_ms'] for x in items)
            if all('startup_ms' in x for x in items) else None)
a.output.write_text(json.dumps(result, indent=2) + '\n')
if any('startup_ms' not in x for x in result['samples']):
    raise SystemExit('one or more launches did not show an editor window')
