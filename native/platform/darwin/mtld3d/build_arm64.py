#!/usr/bin/env python3
"""Build the pinned mtld3d unix side into a new private directory (no Wine SDK required)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

PIN = '9919759fa7c03362deac1b985db6d93de9e33ecf'
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if destination.exists() or source in destination.parents:
        parser.error('destination must be new and outside the source')
    if subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        parser.error('source must be mtld3d v0.15.1 at ' + PIN)
    if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no'], text=True).strip():
        parser.error('source has tracked modifications')
    if shutil.disk_usage(destination.parent).free < 10 * 1024**3:
        parser.error('need 10 GiB free before building to preserve the 8 GiB reserve')
    destination.mkdir()
    # Copy tracked inputs only; do not modify an existing source build or SDK.
    files = subprocess.check_output(['git', '-C', str(source), 'ls-files', '-z']).decode().split('\0')
    for name in filter(None, files):
        target = destination / 'src' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, target)
    patch = HERE / 'patches/0001-raven-iosurface-wow64.patch'
    shutil.copy2(patch, destination / patch.name)
    subprocess.run(['git', 'apply', str(patch)], cwd=destination / 'src', check=True)
    # Capture every adapter input alongside the artifacts for review/rebuild.
    adapter_inputs = ('mtld3d/presentation.m', 'dxmt/presentation.m', 'guest/protocol.h')
    for name in adapter_inputs:
        target = destination / 'adapter' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HERE.parent / name, target)
    adapter_command = ['clang', '-arch', 'arm64', '-dynamiclib', '-O2', '-g', '-fblocks',
                       str(destination / 'adapter/mtld3d/presentation.m'),
                       '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
                       '-framework', 'QuartzCore', '-o', str(destination / 'raven-mtld3d.dylib')]
    subprocess.run(adapter_command, check=True)
    command = ['cargo', 'build', '--manifest-path', str(destination / 'src/unix/Cargo.toml'),
               '-p', 'mtld3d-unix', '--release', '--locked', '-j8']
    with (destination / 'build.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True,
                       env=dict(os.environ, RUSTFLAGS='-C link-arg=-Wl,-undefined,dynamic_lookup'))
    library = destination / 'mtld3d.so'
    shutil.copy2(destination / 'src/unix/target/release/libmtld3d_unix.dylib', library)
    architecture = subprocess.check_output(['lipo', '-archs', str(library)], text=True).strip()
    if architecture != 'arm64':
        raise SystemExit('expected arm64, got ' + architecture)
    (destination / 'unix-call-count').write_text('22\n')
    shutil.copy2(source / 'LICENSE', destination / 'LICENSE')
    (destination / 'artifacts.json').write_text(json.dumps({
        'source': PIN, 'command': command, 'adapter_command': adapter_command,
        'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
        'adapter_sources': {name: hashlib.sha256((HERE.parent / name).read_bytes()).hexdigest() for name in adapter_inputs},
        'rustc': subprocess.check_output(['rustc', '--version'], text=True).strip(),
        'files': {name: hashlib.sha256((destination / name).read_bytes()).hexdigest()
                  for name in ('mtld3d.so', 'raven-mtld3d.dylib', 'unix-call-count', 'LICENSE')},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
