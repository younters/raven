#!/usr/bin/env python3
"""Run either PE width through FEX and verify D3D9 GPU readback, independently of the desktop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import zlib


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_frame(data, frame):
    if len(data) != 256 * 256 * 4:
        raise ValueError('readback must be 256x256 RGBA8')
    checked = triangle = mismatches = 0
    for y in range(256):
        for x in range(256):
            px, py = x + .5, y + .5
            edge, delta = (py - 24) * .5, abs(px - 128)
            inside = 26 < py < 230 and delta < edge - 2
            outside = py < 22 or py > 234 or delta > edge + 2
            if not inside and not outside:
                continue  # Exclude rasterizer edge rules, not triangle interiors.
            expected = bytes((255, 0, 0, 255) if inside else (0, frame * 4, 64, 255))
            offset = (y * 256 + x) * 4
            mismatches += data[offset:offset + 4] != expected
            checked += 1
            triangle += inside
    result = dict(checked=checked, triangle=triangle, mismatches=mismatches)
    if mismatches:
        raise ValueError(f'frame {frame}: {result}')
    return result


def png(path, data):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    rows = b''.join(b'\0' + data[y * 1024:(y + 1) * 1024] for y in range(256))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 256, 256, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--raven', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--width', type=int, choices=(32, 64), required=True)
    parser.add_argument('--modules', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, default=os.environ.get('RAVEN_MTLD3D_ARM64_ROOT'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--capture', action='store_true')
    args = parser.parse_args()
    if not args.artifacts:
        print('SKIP: set RAVEN_MTLD3D_ARM64_ROOT to the private overlay with pe32/pe64 DLLs')
        return 77
    root = args.artifacts.resolve()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix=f'raven-mtld3d{args.width}-'))
    out.mkdir(parents=True, exist_ok=True)
    prefix, app = out / 'prefix', out / 'app'
    # Refuse stale success/readback files; each run owns a new prefix.
    if prefix.exists() or app.exists():
        parser.error('output already contains a previous run')
    app.mkdir()
    for name in ('d3d9.dll', 'mtld3d.dll', 'vcruntime140.dll'):
        shutil.copy2(root / f'pe{args.width}' / name, app / name)
    shutil.copy2(args.fixture, app / 'window.exe')
    (app / 'mtld3d.conf').write_text('shader.asyncCompile = false\n')
    env = dict(os.environ, RAVEN_PREFIX_SETUP='1', RAVEN_PREFIX=str(prefix),
               RAVEN_MTLD3D_ARM64_ROOT=str(root), RAVEN_FEX_MODULE_ROOT=str(args.modules.resolve()))
    command = [str(args.raven.resolve()), '--run-process', '--execution=fex', '--timeout-ms=45000', '--desktop=macos',
               str(app / 'window.exe'), str(app / 'mtld3d.dll'), str(app / 'd3d9.dll')]
    status = prefix / 'drive_c/window.txt'
    captured = False
    with (out / 'run.log').open('w') as log:
        proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 60
            while proc.poll() is None:
                if args.capture and not captured and status.exists() and status.read_text() == 'OK':
                    time.sleep(.5)  # Let the host import the final submitted IOSurface.
                    subprocess.run(['screencapture', '-x', str(out / 'screen.png')], check=True)
                    captured = True
                if time.monotonic() > deadline:
                    raise TimeoutError('fixture exceeded 60 seconds')
                time.sleep(.05)
        finally:
            if proc.poll() is None:
                proc.terminate()  # Only the process started by this run.
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    if proc.returncode or not status.exists() or status.read_text() != 'OK':
        raise RuntimeError(f'fixture failed ({proc.returncode}); inspect {out / "run.log"}')
    result = dict(width=args.width, engine='fex', exit_code=proc.returncode, command=command, frames={}, artifacts={})
    for name, frame in [('first', 0), ('last', 59)]:
        source = prefix / f'drive_c/window_{name}.raw'
        data = source.read_bytes()
        result['frames'][name] = dict(check_frame(data, frame), sha256=sha(source))
        shutil.copy2(source, out / source.name)
        png(out / f'readback_{name}.png', data)
    for path in [app / 'window.exe', app / 'd3d9.dll', app / 'mtld3d.dll', app / 'vcruntime140.dll',
                 root / 'mtld3d.so', root / 'raven-mtld3d.dylib', args.modules / ('xtajit.dll' if args.width == 32 else 'xtajit64.dll')]:
        result['artifacts'][path.name] = sha(path)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: PE{args.width} clear, triangle, 60 presents, first/last GPU readback; {out / "result.json"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
