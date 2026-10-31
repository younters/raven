#!/usr/bin/env python3
"""Build the pinned Mullion source into a NEW private overlay, never shared components.
Requires llvm-mingw on PATH, a configured Wine tree with winecrt0/ntdll/dbghelp
import libraries and winebuild, and static LLVM 15 for each native architecture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = '46e6f0508c4d48dd1aec3ac22df4da87cad102a8'

def run(*args):
    subprocess.run(list(map(str, args)), check=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--wine', type=Path, required=True)
    p.add_argument('--llvm-arm64', type=Path, required=True)
    p.add_argument('--llvm-x86_64', type=Path, required=True)
    a = p.parse_args()
    source, root = a.source.resolve(), a.root.resolve()
    if subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        p.error('source must be at the documented pin')
    if root.exists():
        p.error('root must be a new private directory')
    root.mkdir(parents=True)
    entries = (source / 'src/winemetal/unix/winemetal_unix.c').read_text().split('const void *__wine_unix_call_funcs[] = {')[1].split('};')[0]
    entries = [v.strip() for v in entries.split(',') if v.strip()]
    if len(entries) != 197 or hashlib.sha256('\n'.join(entries[:132]).encode()).hexdigest() != '8095cdebf07b394c726339570d1cc1471c1694d9dc2e55141ba77a59ec2317c9':
        p.error('unexpected unix table layout')
    here = Path(__file__).resolve().parent
    # Patch only the caller's private checkout, retaining the exact patch for provenance.
    run('git', '-C', source, 'apply', here / 'raven-adapter.patch')
    for arch, directory, llvm in [('arm64', 'unix-arm64', a.llvm_arm64), ('x86_64', 'unix64', a.llvm_x86_64)]:
        if shutil.disk_usage(root).free < 10 * 1024**3:
            raise SystemExit('less than 10 GiB free; leave at least 8 GiB for the machine')
        dest = root / directory
        dest.mkdir()
        adapter = dest / 'raven-dxmt.dylib'
        run('/usr/bin/clang', '-arch', arch, '-dynamiclib', '-O2', '-fblocks', here.parent / 'dxmt/unix_adapter.m', here.parent / 'dxmt/presentation.m', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface', '-Wl,-install_name,@rpath/raven-dxmt.dylib', '-o', adapter)
        native = root / f'{arch}.ini'
        native.write_text(f"[binaries]\nc = ['/usr/bin/clang', '-arch', '{arch}']\ncpp = ['/usr/bin/clang++', '-arch', '{arch}']\n")
        build = root / f'build-{arch}'
        run('meson', 'setup', build, source, '--cross-file', source / 'build-win64.txt', '--native-file', native, '--buildtype=debugoptimized', f'-Dnative_llvm_path={llvm}', f'-Dwine_build_path={a.wine}', f'-Draven_adapter={adapter}')
        run('ninja', '-C', build, '-j8')
        shutil.copy2(build / 'src/winemetal/unix/winemetal.so', dest / 'winemetal.so')
        run('install_name_tool', '-change', '@rpath/raven-dxmt.dylib', '@loader_path/raven-dxmt.dylib', dest / 'winemetal.so')
        run('codesign', '--force', '--sign', '-', dest / 'winemetal.so')
        (dest / 'unix-call-count').write_text('197\n')
        if arch == 'arm64':
            pe = root / 'pe64'
            pe.mkdir()
            for module, dll in [('d3d12', 'd3d12'), ('d3d12', 'd3d12core'), ('dxgi', 'dxgi'), ('d3d11', 'd3d11'), ('d3d10', 'd3d10core'), ('winemetal', 'winemetal')]:
                shutil.copy2(build / 'src' / module / (dll + '.dll'), pe / (dll + '.dll'))
    shutil.copy2(source / 'LICENSE', root / 'Mullion.LICENSE')
    files = {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest() for directory in ['pe64', 'unix64', 'unix-arm64'] for f in (root / directory).iterdir() if f.is_file()}
    (root / 'artifacts.json').write_text(json.dumps({'source': 'https://github.com/dttdrv/mullion', 'commit': PIN, 'license': 'LGPL-2.1-or-later', 'files': files}, indent=2)+'\n')

if __name__ == '__main__':
    main()
