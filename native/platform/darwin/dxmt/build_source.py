"""Experimental pinned DXMT source build; publish only to a private provisioner overlay."""
from pathlib import Path
import hashlib
import shutil
import subprocess

REVISION = '3b252590cb0290e36fca6f088b5255b871d2d95a'
KEYED_MUTEX_MERGE = 'e520fea415ba4b82b0c346dae77bbb1be4897453'


def run(*args, cwd=None):
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


def build(root, llvm, wine):
    here = Path(__file__).resolve().parent
    work = root.parent / 'dxmt-source-build'
    source, output = work / 'src', work / 'build'
    work.mkdir(parents=True, exist_ok=True)
    if not (source / '.git').exists():
        run('git', 'clone', 'https://github.com/3Shain/dxmt', source)
    run('git', 'fetch', 'origin', REVISION, cwd=source)
    # This checkout belongs exclusively to the provisioner, never to a developer.
    run('git', 'reset', '--hard', REVISION, cwd=source)
    run('git', 'merge-base', '--is-ancestor', KEYED_MUTEX_MERGE, REVISION, cwd=source)
    run('git', 'submodule', 'update', '--init', '--recursive', cwd=source)
    for patch in sorted((here / 'patches').glob('*.patch')):
        run('git', 'apply', patch, cwd=source)
    table = (source / 'src/winemetal/unix/winemetal_unix.c').read_text().split(
        'const void *__wine_unix_call_funcs[] = {', 1)[1].split('};', 1)[0]
    entries = [entry.strip() for entry in table.split(',') if entry.strip()]
    if len(entries) != 146 or hashlib.sha256('\n'.join(entries[:132]).encode()).hexdigest() != '8095cdebf07b394c726339570d1cc1471c1694d9dc2e55141ba77a59ec2317c9':
        raise RuntimeError('DXMT unix table no longer matches the Raven wrapper ABI')
    native = work / 'native.ini'
    native.write_text("[binaries]\nc = ['/usr/bin/clang', '-arch', 'x86_64']\n"
                      "cpp = ['/usr/bin/clang++', '-arch', 'x86_64']\n")
    run('meson', 'setup', *(['--reconfigure'] if (output / 'meson-private').exists() else []),
        output, source, '--cross-file', source / 'build-win64.txt', '--native-file', native,
        '--buildtype=release', '-Denable_tests=true', f'-Dnative_llvm_path={llvm}',
        f'-Dwine_build_path={wine}', f'-Draven_adapter={root / "unix64/raven-dxmt.dylib"}')
    run('meson', 'compile', '-C', output, '-j4')
    for component, name in [('d3d11', 'd3d11'), ('dxgi', 'dxgi'), ('d3d10', 'd3d10core'), ('winemetal', 'winemetal')]:
        shutil.copy2(output / 'src' / component / (name + '.dll'), root / 'pe64' / (name + '.dll'))
    library = root / 'unix64/winemetal.so'
    shutil.copy2(output / 'src/winemetal/unix/winemetal.so', library)
    run('/usr/bin/install_name_tool', '-change', '@rpath/raven-dxmt.dylib', '@loader_path/raven-dxmt.dylib', library)
    run('/usr/bin/codesign', '--force', '--sign', '-', library)
    (root / 'unix64/unix-call-count').write_text('146\n')
    return {'unix_call_count': 146, 'commit': REVISION, 'keyed_mutex_merge': KEYED_MUTEX_MERGE,
            'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((here / 'patches').glob('*.patch'))}}
