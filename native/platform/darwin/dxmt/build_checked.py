#!/usr/bin/env python3
"""Rebuild pinned unix readers in a private overlay, reusing immutable native archives."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
from harden_source import harden

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--archives', type=Path)
    parser.add_argument('--arch', choices=('arm64', 'x86_64'), required=True)
    parser.add_argument('--backend', choices=('dxmt', 'mullion'), required=True)
    args = parser.parse_args()
    source, original, root = (p.resolve() for p in (args.source, args.build, args.destination))
    if root.exists() or source in root.parents or original in root.parents:
        parser.error('destination must be new and outside the source/build')
    if shutil.disk_usage(root.parent).free < 10 * 1024**3:
        parser.error('need 10 GiB free to preserve the 8 GiB reserve')
    root.mkdir()
    private = root / 'source'
    shutil.copytree(source, private, ignore=shutil.ignore_patterns('.git', 'build', '__pycache__'))
    count = harden(private)
    if count != (197 if args.backend == 'mullion' else 146):
        raise ValueError('unqualified source pin')
    shutil.copy2(HERE / 'macdrv_abi.h', private / 'src/winemetal/unix/raven_macdrv_abi.h')
    shutil.copy2(HERE / 'checked_memory.h', private / 'src/winemetal/unix/raven_checked_memory.h')
    shutil.copy2(HERE.parent / 'unix_memory.h', private / 'src/winemetal/unix_memory.h')
    build = root / 'build'
    build.mkdir()
    for path in original.rglob('*.h'):
        out = build / path.relative_to(original)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)
    manifest = json.loads((original / 'compile_commands.json').read_text())
    selected = [c for c in manifest if 'winemetal.so.p/' in c['output'] or c['output'].endswith('darwin/libairconv.a.p/.._dxbc_converter.cpp.o')]
    prefix = '../../raven-mullion' if args.backend == 'mullion' else '../src'
    def rewrite(command):
        words = shlex.split(command)
        if words[0].endswith('/ccache'): words.pop(0)
        for i, word in enumerate(words):
            if i and words[i-1] == '-arch': words[i] = args.arch
            elif word.endswith('/raven-dxmt.dylib') and not word.startswith('@'):
                words[i] = str(root / 'raven-dxmt.dylib')
            else:
                value = word.replace(prefix, str(private))
                if args.arch == 'arm64':
                    value = value.replace('/Users/yount/Documents/Projects/raven-components-build/llvm15', '/opt/homebrew/opt/llvm@15')
                words[i] = value

        return words
    adapter = ['/usr/bin/clang', '-arch', args.arch, '-dynamiclib', '-O2', '-g', '-fblocks',
               str(HERE / 'unix_adapter.m'), str(HERE / 'presentation.m'), str(HERE / 'checked_memory.c')]
    if args.backend == 'mullion': adapter.append(str(HERE.parent / 'mullion/unreachable.c'))
    adapter += ['-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
                '-Wl,-install_name,@rpath/raven-dxmt.dylib', '-o', str(root / 'raven-dxmt.dylib')]
    subprocess.run(adapter, check=True)
    for entry in selected:
        (build / entry['output']).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(rewrite(entry['command']), cwd=build, check=True)
    for name in ('src/airconv/darwin/libairconv.a', 'libs/DXBCParser/libDXBCParserNative.a'):
        out = build / name
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2((args.archives or original) / name, out)
        if 'libairconv.a' in name:
            subprocess.run(['ar', 'r', str(out)] + [str(build/c['output']) for c in selected if 'libairconv.a.p/' in c['output']], check=True)
    link = subprocess.check_output(['ninja', '-C', str(original), '-t', 'commands', 'src/winemetal/unix/winemetal.so'], text=True).splitlines()[-1]
    subprocess.run(rewrite(link) + (['/opt/homebrew/opt/zstd/lib/libzstd.a'] if args.arch == 'arm64' else []), cwd=build, check=True)
    library = root / 'winemetal.so'
    shutil.copy2(build / 'src/winemetal/unix/winemetal.so', library)
    subprocess.run(['install_name_tool', '-change', '@rpath/raven-dxmt.dylib', '@loader_path/raven-dxmt.dylib', str(library)], check=True)
    dependencies = '\n'.join(subprocess.check_output(['otool', '-L', str(library)], text=True).splitlines()[1:])
    if '@loader_path/raven-dxmt.dylib (' not in dependencies or str(root) in dependencies:
        raise ValueError('adapter dependency must be loader-relative')
    subprocess.run(['codesign', '--force', '--sign', '-', str(library)], check=True)
    (root / 'unix-call-count').write_text(f'{count}\n')
    (root / 'artifacts.json').write_text(json.dumps(dict(source=str(source), build=str(original), arch=args.arch,
        files={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ('winemetal.so','raven-dxmt.dylib','unix-call-count')}), indent=2)+'\n')

if __name__ == '__main__': main()
