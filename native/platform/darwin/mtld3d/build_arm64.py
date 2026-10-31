#!/usr/bin/env python3
"""Build the pinned mtld3d unix side into a new private directory (no Wine SDK required)."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = '9919759fa7c03362deac1b985db6d93de9e33ecf'


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
    command = ['cargo', 'build', '--manifest-path', str(destination / 'src/unix/Cargo.toml'),
               '-p', 'mtld3d-unix', '--release', '--locked', '-j8']
    with (destination / 'build.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    library = destination / 'mtld3d.so'
    shutil.copy2(destination / 'src/unix/target/release/libmtld3d_unix.dylib', library)
    architecture = subprocess.check_output(['lipo', '-archs', str(library)], text=True).strip()
    if architecture != 'arm64':
        raise SystemExit('expected arm64, got ' + architecture)
    (destination / 'unix-call-count').write_text('22\n')
    shutil.copy2(source / 'LICENSE', destination / 'LICENSE')
    (destination / 'artifacts.json').write_text(json.dumps({
        'source': PIN, 'command': command,
        'rustc': subprocess.check_output(['rustc', '--version'], text=True).strip(),
        'files': {name: hashlib.sha256((destination / name).read_bytes()).hexdigest()
                  for name in ('mtld3d.so', 'unix-call-count', 'LICENSE')},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
