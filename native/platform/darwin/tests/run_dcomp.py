#!/usr/bin/env python3
"""Check actual DirectComposition pixels, cross-process parenting, and commits without new presents."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from PIL import Image


def green_box(path):
    image = Image.open(path).convert('RGB')
    # Window screenshots may contain two physical pixels per logical pixel.
    pixels = image.load()
    points = [(x, y) for y in range(image.height) for x in range(image.width)
              if pixels[x, y][1] > 220 and pixels[x, y][0] < 50 and pixels[x, y][2] < 50]
    if len(points) < 100:
        return None
    xs, ys = zip(*points)
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def main():
    raven, program, root, info_tool, work = map(lambda s: Path(s).resolve(), sys.argv[1:6])
    kind = sys.argv[6] if len(sys.argv) > 6 else 'local'
    foreign = kind.startswith('foreign')
    hidpi = kind.endswith('hidpi')
    if not (root / 'pe64/dcomp.dll').is_file():
        print('source DXMT overlay with DirectComposition required')
        return 77
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    prefix = work / 'prefix'
    env = os.environ | {'RAVEN_PREFIX': str(prefix), 'RAVEN_DXMT_ROOT': str(root), 'RAVEN_TRACE_DXMT': '1'}
    command = [str(raven), '--run-process', '--execution=native', '--timeout-ms=45000', '--desktop=macos', str(program)]
    if foreign or hidpi:
        command += ['--']
    if foreign:
        command += ['--foreign']
    if hidpi:
        command += ['--hidpi']
    boxes = []
    with (work / 'run.log').open('w') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            for stage in (1, 2):
                deadline = time.monotonic() + 30
                ready = prefix / f'drive_c/dcomp-stage{stage}'
                while not ready.exists():
                    if process.poll() is not None:
                        raise RuntimeError(f'fixture exited {process.returncode} before stage {stage}')
                    if time.monotonic() > deadline:
                        raise RuntimeError(f'timed out before stage {stage}')
                    time.sleep(.05)
                # A committed visual update propagates without a new swap-chain Present.
                time.sleep(.3)
                info = json.loads(subprocess.check_output([str(info_tool), str(process.pid)]))
                shot = work / f'stage{stage}.png'
                subprocess.run(['/usr/sbin/screencapture', '-x', '-o', '-l', str(info['id']), str(shot)],
                               check=True, capture_output=True)
                box = green_box(shot)
                if not box:
                    raise RuntimeError(f'no rendered green surface in stage {stage}')
                scale = Image.open(shot).width / info['bounds']['Width']
                # DPI-aware guests use one Windows pixel per physical screenshot pixel.
                # Unaware guests use logical coordinates and are enlarged by the backing scale.
                unit = 1 if hidpi else scale
                box = tuple(v / unit for v in box)
                if abs((box[2] - box[0]) - 200) > 2 or abs((box[3] - box[1]) - 120) > 2:
                    raise RuntimeError(f'incorrect surface size: {box}')
                boxes.append(box)
                (prefix / f'drive_c/dcomp-next{stage}').write_text('OK')
            if process.wait(timeout=10):
                raise RuntimeError(f'fixture exited {process.returncode}')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    delta = boxes[1][0] - boxes[0][0], boxes[1][1] - boxes[0][1]
    if abs(delta[0] - 55) > 2 or abs(delta[1] - 40) > 2:
        raise RuntimeError(f'Commit did not move the retained image: {delta}')
    text = (work / 'run.log').read_text()
    if 'presentation errors' not in text or '0 presentation errors' not in text:
        raise RuntimeError('missing successful presentation counters')
    print(f'DirectComposition {"foreign" if foreign else "local"}: screen pixels, retained commit and device event passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
