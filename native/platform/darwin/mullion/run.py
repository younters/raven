#!/usr/bin/env python3
"""Run existing strict D3D12 fixtures with the opt-in matched Mullion stack."""
import os
from pathlib import Path
import subprocess
import sys
import threading

raven, exe, root, work = map(lambda s: Path(s).resolve(), sys.argv[1:5])
kind, engine = sys.argv[5:7]
work.mkdir(parents=True, exist_ok=True)
# Every invocation gets a clean prefix; stale readbacks must never count as success.
import tempfile
prefix = Path(tempfile.mkdtemp(prefix='prefix-', dir=work))
env = os.environ | {'RAVEN_PREFIX': str(prefix), 'RAVEN_D3D12_BACKEND': 'mullion',
                    'RAVEN_MULLION_ROOT': str(root), 'MULLION_D3D12_METAL4': '0'}
finished = threading.Event()
def capture():
    # Only capture while this fixture is alive; readback remains the assertion.
    if not finished.wait(3):
        subprocess.run(['/usr/sbin/screencapture', '-x', str(work / 'screen.png')], timeout=10, check=False)
thread = threading.Thread(target=capture) if kind == 'window12' else None
if thread: thread.start()
result = subprocess.run([str(raven), '--run-process', f'--execution={engine}', '--timeout-ms=80000',
                         '--desktop=macos', str(exe)], cwd=work, env=env, capture_output=True, timeout=90)
finished.set()
if thread: thread.join()
(work / 'stdout.log').write_bytes(result.stdout)
(work / 'stderr.log').write_bytes(result.stderr)
drive = prefix / 'drive_c'
report = drive / ('d3d12.txt' if kind == 'device' else kind + '.txt')
text = report.read_text() if report.exists() else 'missing'
if result.returncode or ('query=ok' not in text if kind == 'device' else text != 'OK'):
    raise SystemExit(f'{engine}/{kind}: exit={result.returncode}, report={text!r}\n{result.stderr[-3000:].decode(errors="replace")}')
if kind == 'device':
    fields = dict(item.split('=', 1) for item in text.split() if '=' in item)
    assert fields['hr'] == '00000000' and int(fields['level'], 16) >= 0xb000, text
elif kind == 'clear':
    assert (drive / 'clear.raw').read_bytes() == bytes((51, 102, 153, 255)) * 4096
elif kind in ('draw', 'window12'):
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[4] / 'scripts/check-d3d11-triangle.py'), str(drive / ('draw.raw' if kind == 'draw' else 'window12_last.raw'))], check=True)
print(f'{engine}/{kind}: {text}')
