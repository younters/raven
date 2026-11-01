#!/usr/bin/env python3
"""Build a private arm64 unix overlay from an existing pinned DXMT source build.

Reuse its generated Metal headers and native compile manifest; never run Meson
regeneration or write into the supplied source build. PE DLLs remain unchanged.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
from harden_source import harden


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-build', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--llvm', type=Path, default=Path('/opt/homebrew/opt/llvm@15'))
    args = parser.parse_args()
    source_build, root, llvm = (p.resolve() for p in (args.source_build, args.destination, args.llvm))
    if root.exists():
        raise SystemExit('destination must be a new private directory')
    original = source_build / 'build'
    manifest = json.loads((original / 'compile_commands.json').read_text())
    selected = [c for c in manifest if any(s in c['output'] for s in (
        'darwin/libairconv.a.p/', 'libDXBCParserNative.a.p/', 'winemetal.so.p/'))]
    if not selected:
        raise SystemExit('native DXMT compile commands missing')
    root.mkdir(parents=True)
    shutil.copytree(source_build / 'src', root / 'src')
    build = root / 'build'
    build.mkdir()
    for path in original.rglob('*.h'):
        output = build / path.relative_to(original)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, output)
    table_text = (root / 'src/src/winemetal/unix/winemetal_unix.c').read_text()
    entries = [v.strip() for v in table_text.split('const void *__wine_unix_call_funcs[] = {', 1)[1].split('};', 1)[0].split(',') if v.strip()]
    if len(entries) != 146 or hashlib.sha256('\n'.join(entries[:132]).encode()).hexdigest() != '8095cdebf07b394c726339570d1cc1471c1694d9dc2e55141ba77a59ec2317c9':
        raise SystemExit('expected the pinned 146-entry source overlay')
    harden(root / 'src')
    here = Path(__file__).resolve().parent
    shutil.copy2(here / 'checked_memory.h', root / 'src/src/winemetal/unix/raven_checked_memory.h')
    shutil.copy2(here / 'macdrv_abi.h', root / 'src/src/winemetal/unix/raven_macdrv_abi.h')
    shutil.copy2(here.parent / 'unix_memory.h', root / 'src/src/winemetal/unix_memory.h')
    (root / 'unix-call-count').write_text(f'{len(entries)}\n')
    here = Path(__file__).resolve().parent
    adapter = root / 'raven-dxmt.dylib'
    subprocess.run(['/usr/bin/clang', '-arch', 'arm64', '-dynamiclib', '-O2', '-Wall', '-Wextra', '-fblocks',
                    str(here / 'unix_adapter.m'), str(here / 'presentation.m'), str(here / 'checked_memory.c'), '-framework', 'Foundation',
                    '-framework', 'Metal', '-framework', 'IOSurface',
                    '-Wl,-install_name,@rpath/raven-dxmt.dylib', '-o', str(adapter)], check=True)

    def rewrite(command):
        words = shlex.split(command)
        for i, word in enumerate(words):
            if i and words[i - 1] == '-arch':
                words[i] = 'arm64'
            elif not word.startswith('@') and word.endswith('/raven-dxmt.dylib'):
                words[i] = str(adapter)
            elif word.startswith('-I') and word.endswith('/llvm15/include'):
                words[i] = '-I' + str(llvm / 'include')
            elif word.startswith('-L') and word.endswith('/llvm15/lib'):
                words[i] = '-L' + str(llvm / 'lib')
        return words

    def compile_one(command):
        (build / command['output']).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(rewrite(command['command']), cwd=build, check=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(compile_one, selected))
    for library in ('src/airconv/darwin/libairconv.a', 'libs/DXBCParser/libDXBCParserNative.a'):
        objects = [c['output'] for c in selected if c['output'].startswith(library + '.p/')]
        subprocess.run(['ar', 'rcs', library] + objects, cwd=build, check=True)
    link = subprocess.check_output(['ninja', '-C', str(original), '-t', 'commands',
                                    'src/winemetal/unix/winemetal.so'], text=True).splitlines()[-1]
    subprocess.run(rewrite(link) + ['/opt/homebrew/opt/zstd/lib/libzstd.a'], cwd=build, check=True)
    library = root / 'winemetal.so'
    shutil.copy2(build / 'src/winemetal/unix/winemetal.so', library)
    subprocess.run(['install_name_tool', '-change', '@rpath/raven-dxmt.dylib',
                    '@loader_path/raven-dxmt.dylib', str(library)], check=True)
    dependencies = '\n'.join(subprocess.check_output(['otool', '-L', str(library)], text=True).splitlines()[1:])
    if '@loader_path/raven-dxmt.dylib (' not in dependencies or str(root) in dependencies:
        raise SystemExit('winemetal dependency is not loader-relative')
    subprocess.run(['codesign', '--force', '--sign', '-', str(library)], check=True)
    files = [library, adapter, root / 'unix-call-count']
    (root / 'artifacts.json').write_text(json.dumps({
        'source_build': str(source_build), 'llvm': str(llvm),
        'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
