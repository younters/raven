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
                # Wait for this stage's pixels, including updates without a new Present.
                deadline = time.monotonic() + 5
                while True:
                    selected_window = [str(info_tool), str(process.pid)]
                    if kind.startswith('popup'):
                        selected_window += ['popup']
                    info = json.loads(subprocess.check_output(selected_window))
                    shot = work / f'stage{stage}.png'
                    capture = subprocess.run(['/usr/sbin/screencapture', '-x', '-o', '-l', str(info['id']), str(shot)],
                                             capture_output=True)
                    if capture.returncode or not shot.is_file():
                        print('screen capture unavailable:', capture.stderr.decode(errors='replace'))
                        return 77
                    try:
                        image = Image.open(shot).convert('RGB')
                        box = green_box(shot)
                    except OSError as error:
                        print('screen capture unavailable:', error)
                        return 77
                    scale = image.width / info['bounds']['Width']
                    unit = 1 if hidpi else scale
                    if box:
                        box = tuple(v / unit for v in box)
                        expected = (240, 50) if stage == 4 else (200, 120)
                        matches = (abs(box[2] - box[0] - expected[0]) <= 2 and
                                   abs(box[3] - box[1] - expected[1]) <= 2)
                        if stage == 2:
                            matches &= all(abs(a - b) <= 2 for a, b in zip(box, boxes[0]))
                        if stage == 3:
                            matches &= abs(box[0] - boxes[0][0] - 55) <= 2 and abs(box[1] - boxes[0][1] - 40) <= 2
                        if stage == 4:
                            matches &= abs(box[0] - boxes[2][0] - 30) <= 2 and abs(box[1] - boxes[2][1] - 5) <= 2
                        if stage >= 5:
                            origin = boxes[2][0] - 80, boxes[2][1] - 60
                            def sample(x, y):
                                return image.getpixel((round((origin[0] + x) * unit), round((origin[1] + y) * unit)))
                            if stage == 5:
                                r, g, _ = sample(82, 62)
                                matches &= not (g > 220 and r < 50) and sample(110, 64)[1] >= 220
                            if stage == 6:
                                r, g, b = sample(150, 100)
                                matches &= r >= 70 and g >= 70 and b <= 60 and abs(r - g) <= 55
                            if stage >= 7:
                                r, g, b = sample(150, 100)
                                matches &= r <= 50 and g >= 220 and b <= 50
                            if stage == 8:
                                matches &= image.width >= images[6].width * 1.4
                        if matches:
                            break
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f'expected pixels did not arrive at stage {stage}: {box}')
                    time.sleep(.05)
                boxes.append(box)
                images.append(Image.open(shot).convert('RGB'))
                units.append(unit)
                (prefix / f'drive_c/dcomp-next{stage}').write_text('OK')
            if kind in ('local', 'local-hidpi'):
                old_info = info
                ready = prefix / 'drive_c/dcomp-stage9'
                deadline = time.monotonic() + 30
                while not ready.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('fixture did not reach retarget stage')
                    time.sleep(.05)
                deadline = time.monotonic() + 5
                while True:
                    new_info = json.loads(subprocess.check_output([str(info_tool), str(process.pid), 'Raven retarget']))
                    found = []
                    for name, selected in [('old', old_info), ('new', new_info)]:
                        shot = work / f'retarget-{name}.png'
                        result = subprocess.run(['/usr/sbin/screencapture', '-x', '-o', '-l', str(selected['id']), str(shot)],
                                                capture_output=True)
                        if result.returncode or not shot.is_file():
                            print('screen capture unavailable during retarget')
                            return 77
                        found.append(green_box(shot))
                    if found[0] is None and found[1] is not None:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f'retarget did not detach old layer and display new frame: {found}')
                    time.sleep(.05)
                (prefix / 'drive_c/dcomp-next9').write_text('OK')
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
