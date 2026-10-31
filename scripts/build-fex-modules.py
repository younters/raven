#!/usr/bin/env python3
"""Build pinned PE modules and native host externally (always -j8).
Run test-fex-modhost.py separately to require guest execution and measurements.
"""
import argparse
import re
import hashlib
import json
import os
import tempfile
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from fex_modhost_contract import verify_patch_pins, verify_module_pin, verify_wow64_module_pin

PIN = '370c63057fc9fc826b58fe15d59930876df55b5f'
URL = 'https://github.com/dbc-hbin/FEX-darwin.git'
SOURCE_SHA = '1e2dcfc3e7946f9a6db664e50fc2f9513060f0663c12ddbbeaeba336cabf9fcd'
TOOL = 'llvm-mingw-20260616-ucrt-macos-universal'
TOOL_SHA = '2cab02a2e964bd4aae981150a45985d07c657cfa8d244959eb9e2dcc5eedd7b1'
SUBMODULES = ['External/fmt', 'External/range-v3', 'External/rpmalloc',
              'External/unordered_dense', 'External/xxhash', 'Source/Common/cpp-optparse']
REPO = Path(__file__).resolve().parents[1]

def run(argv, **kw):
    print('+', ' '.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kw)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def probes(root):
    out = root / 'gate1'
    out.mkdir(exist_ok=True)
    results = []
    for name, flags in [('default', []), ('min11', ['-mmacosx-version-min=11.0']),
                        ('sdk11', ['-Wl,-platform_version,macos,11.0,11.0'])]:
        exe = out / ('x18-' + name)
        run(['xcrun', 'clang', '-arch', 'arm64', '-O2', '-Wall', '-Wextra', '-ffixed-x18',
             *flags, REPO / 'native/platform/darwin/fex/modhost/x18_probe.c', '-o', exe])
        # Deliberately failing default and public-toggle controls run in child processes.
        for args in [[], ['--enable']]:
            p = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=10)
            results.append(dict(variant=name, args=args, returncode=p.returncode,
                                stdout=p.stdout, stderr=p.stderr, sha256=sha(exe)))
            print(json.dumps(results[-1]), flush=True)
        for cmd, suffix in [(['otool', '-l'], 'load-commands'),
                            (['codesign', '-d', '--entitlements', ':-'], 'signature')]:
            p = subprocess.run([*cmd, str(exe)], capture_output=True, text=True)
            (out / (name + '-' + suffix + '.txt')).write_text(p.stdout + p.stderr)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if next(r for r in results if r['variant'] == 'sdk11' and not r['args'])['returncode']:
        raise RuntimeError('x18 legacy SDK gate failed; stop before module build')

def build(root, raven_host=False, build_id=None, clean=False):
    contract = verify_patch_pins() if raven_host else None
    source = root / 'FEX-darwin'
    if not source.exists():
        run(['git', 'clone', '--no-checkout', URL, source])
        run(['git', '-C', source, 'checkout', '--detach', PIN])
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != PIN:
        raise RuntimeError('source HEAD does not match pin; refusing to change existing checkout')
    archive_sha = hashlib.sha256(subprocess.check_output(
        ['git', '-C', str(source), 'archive', '--format=tar', PIN])).hexdigest()
    if archive_sha != SOURCE_SHA:
        raise RuntimeError('source archive checksum mismatch')
    # Verify any tracked edits match only the exact Raven patch series.
    patch_dir = 'native/platform/darwin/fex/patches' if raven_host else 'native/platform/darwin/fex/modhost/patches'
    patches = sorted((REPO / patch_dir).glob('*.patch'))
    # Validate a clean/prefix/fully patched tree before changing external sources.
    # Private indexes handle overlapping hunks and new headers without staging them.
    with tempfile.TemporaryDirectory(prefix='raven-fex-index-') as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / 'index'))
        run(['git', '-C', source, 'read-tree', PIN], env=env)
        trees = [subprocess.check_output(['git', '-C', str(source), 'write-tree'], env=env)]
        for patch in patches:
            run(['git', '-C', source, 'apply', '--cached', patch], env=env)
            trees.append(subprocess.check_output(['git', '-C', str(source), 'write-tree'], env=env))
        run(['git', '-C', source, 'read-tree', PIN], env=env)
        run(['git', '-C', source, 'add', '-A'], env=env)
        actual = subprocess.check_output(['git', '-C', str(source), 'write-tree'], env=env)
        if actual not in trees:
            raise RuntimeError('unexpected edits in source checkout')
        for patch in patches[trees.index(actual):]:
            run(['git', '-C', source, 'apply', patch])
    run(['git', '-C', source, 'submodule', 'update', '--init', '--jobs', '8', *SUBMODULES])
    archive = root / (TOOL + '.tar.xz')
    if not archive.exists():
        urllib.request.urlretrieve('https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/' + archive.name, archive)
    if sha(archive) != TOOL_SHA:
        raise RuntimeError('llvm-mingw archive checksum mismatch')
    compiler = root / TOOL
    if not compiler.exists():
        with tarfile.open(archive) as stream:
            # Archive is authenticated above; preserve toolchain's internal symlinks.
            stream.extractall(root, filter='data')
    build_id = build_id or ('m1d-final' if raven_host else 'm1b')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', build_id):
        raise RuntimeError('build id must be a simple directory name')
    output = source / 'build/local' / build_id
    if clean and output.exists():
        if output.is_symlink():
            raise RuntimeError('refusing to clean a symlink build output')
        shutil.rmtree(output)
    manifest = output / 'fex-result.json'
    cached = manifest.exists()
    print(f'Build id={build_id} mode={"cached" if cached else "fresh"}', flush=True)
    if not cached:
        run([sys.executable, source / 'Darwin/build.py', '--output', output,
             '--llvm-mingw', compiler, '--jobs', '8', *(['--raven-host'] if raven_host else [])])
    result = json.loads(manifest.read_text())
    status = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all'], text=True)
    if result['source_head'] != PIN or result['source_status'] != status:
        raise RuntimeError('cached build source identity mismatch')
    for item in result['submodules']:
        sub = source / item['path']
        actual_pin = subprocess.check_output(['git', '-C', str(sub), 'rev-parse', 'HEAD'], text=True).strip()
        clean = subprocess.check_output(['git', '-C', str(sub), 'status', '--porcelain'], text=True).strip()
        item['archive_sha256'] = hashlib.sha256(subprocess.check_output(
            ['git', '-C', str(sub), 'archive', '--format=tar', actual_pin])).hexdigest()
        if actual_pin != item['commit'] or clean:
            raise RuntimeError('submodule identity mismatch: ' + item['path'])
    if result['status'] != 'pass':
        raise RuntimeError('build blocked; inspect ' + str(manifest))
    for item in result['licenses']:
        bundled = output / 'licenses' / item['path'].replace('/', '__')
        if sha(bundled) != item['sha256']:
            raise RuntimeError('license bundle checksum mismatch: ' + item['path'])
    shutil.copytree(output / 'licenses', root / 'licenses', dirs_exist_ok=True)
    for name, info in result['artifacts'].items():
        src = Path(info['path'])
        if sha(src) != info['sha256']:
            raise RuntimeError('artifact checksum mismatch: ' + name)
        if raven_host and name == 'xtajit64.dll':
            verify_module_pin(src, contract)
        if raven_host and name == 'xtajit.dll':
            verify_wow64_module_pin(src, contract)
        shutil.copyfile(src, root / name)
    pins = dict(source_commit=PIN, source_archive_sha256=hashlib.sha256(subprocess.check_output(
        ['git', '-C', str(source), 'archive', '--format=tar', PIN])).hexdigest(),
        toolchain_archive_sha256=TOOL_SHA, patches=[dict(path=str(p.relative_to(REPO)), sha256=sha(p)) for p in patches],
        build_id=build_id, cached=cached, build=result)
    (root / ('m1d-manifest.json' if raven_host else 'manifest.json')).write_text(json.dumps(pins, indent=2) + '\n')
    exe = root / 'pe-preflight'
    run(['xcrun', 'clang++', '-arch', 'arm64', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
         REPO / 'native/platform/darwin/fex/modhost/pe_preflight.cpp', '-o', exe])
    for name in result['artifacts']:
        p = subprocess.run([str(exe), str(root / name)], capture_output=True, text=True)
        (root / (name + '-preflight.log')).write_text(p.stdout + p.stderr)
        if p.returncode != 3:
            raise RuntimeError('unexpected preflight outcome: ' + name)
    if raven_host:
        host = root / 'm1d-native-host'
        run(['xcrun', 'clang++', '-arch', 'arm64', '-ffixed-x18', '-ffp-contract=off', '-mmacosx-version-min=11.0', '-std=c++17', '-O2',
             '-Wall', '-Wextra', '-Werror', '-Wl,-platform_version,macos,11.0,11.0',
             REPO / 'native/platform/darwin/fex/modhost/native_host.cpp',
             REPO / 'native/platform/darwin/fex/modhost/abi.S',
             REPO / 'native/platform/darwin/fex/modhost/context.S', '-o', host])
        run([host, '--self-test'])
        p = subprocess.run([str(host), str(root / 'xtajit64.dll')], capture_output=True, text=True)
        (root / 'm1d-host-preflight.log').write_text(p.stdout + p.stderr)
        if p.returncode != 3:
            raise RuntimeError('unexpected native host preflight result')
    print('Module build/audit PASS. Run scripts/test-fex-modhost.py to require runtime execution.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--raven-host', action='store_true', help='Build the Raven host frontend; qualify execution with test-fex-modhost.py')
    parser.add_argument('--root', type=Path, default=os.environ.get('RAVEN_FEX_MODULE_ROOT'))
    parser.add_argument('--build-id', help='Named build output under FEX-darwin/build/local')
    parser.add_argument('--clean', action='store_true', help='Remove only the selected build output before rebuilding')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--probes-only', action='store_true')
    mode.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    if args.root is None:
        parser.error('set RAVEN_FEX_MODULE_ROOT or pass --root')
    args.root.mkdir(parents=True, exist_ok=True)
    try:
        if not args.build_only: probes(args.root)
        if not args.probes_only: build(args.root, args.raven_host, args.build_id, args.clean)
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        sys.exit(str(error))
