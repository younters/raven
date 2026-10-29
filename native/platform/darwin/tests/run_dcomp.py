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
    if foreign or hidpi or kind.startswith('popup'):
        command += ['--']
    if kind.startswith('popup'):
        command += ['--popup']
    if foreign:
        command += ['--foreign']
    if hidpi:
        command += ['--hidpi']
    boxes = []
    images = []
    units = []
    with (work / 'run.log').open('w') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            for stage in range(1, 9):
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
                selected_window = [str(info_tool), str(process.pid)]
                if kind.startswith('popup'):
                    selected_window += ['popup']
                info = json.loads(subprocess.check_output(selected_window))
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
                expected = (240, 50) if stage == 4 else (200, 120)
                if abs((box[2] - box[0]) - expected[0]) > 2 or abs((box[3] - box[1]) - expected[1]) > 2:
                    raise RuntimeError(f'incorrect surface size at stage {stage}: {box}')
                boxes.append(box)
                images.append(Image.open(shot).convert('RGB'))
                units.append(unit)
                (prefix / f'drive_c/dcomp-next{stage}').write_text('OK')
            if process.wait(timeout=10):
                raise RuntimeError(f'fixture exited {process.returncode}')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    delta = boxes[2][0] - boxes[0][0], boxes[2][1] - boxes[0][1]
    if abs(delta[0] - 55) > 2 or abs(delta[1] - 40) > 2:
        raise RuntimeError(f'Commit did not move the retained image: {delta}')
    if any(abs(a - b) > 2 for a, b in zip(boxes[0], boxes[1])):
        raise RuntimeError('pending setters changed a visual before Commit')
    if abs(boxes[3][0] - boxes[2][0] - 30) > 2 or abs(boxes[3][1] - boxes[2][1] - 5) > 2:
        raise RuntimeError('scaled clip is in the wrong coordinate space')
    origin = boxes[2][0] - 80, boxes[2][1] - 60
    def pixel(index, x, y):
        return images[index].getpixel((round((origin[0] + x) * units[index]), round((origin[1] + y) * units[index])))
    if pixel(4, 82, 62)[1] > 220 and pixel(4, 82, 62)[0] < 50:
        raise RuntimeError('rounded clip retained its square corner')
    if pixel(4, 110, 64)[1] < 220:
        raise RuntimeError('rounded clip removed its straight top edge')
    red, green, blue = pixel(5, 150, 100)
    if red < 70 or green < 70 or blue > 60 or abs(red - green) > 55:
        raise RuntimeError(f'premultiplied overlay did not blend over green: {(red, green, blue)}')
    red, green, blue = pixel(6, 150, 100)
    if red > 50 or green < 220 or blue > 50:
        raise RuntimeError('detached visual still covers the retained green image')
    if images[7].width < images[6].width * 1.4 or any(abs(a - b) > 2 for a, b in zip(boxes[7], boxes[6])):
        raise RuntimeError('window resize changed the visual pixel scale or did not resize the window')
    text = (work / 'run.log').read_text()
    if 'presentation errors' not in text or '0 presentation errors' not in text:
        raise RuntimeError('missing successful presentation counters')
    print(f'DirectComposition {"foreign" if foreign else "local"}: screen pixels, atomic/retained commit, scale, clips, alpha, detach, resize and device event passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
