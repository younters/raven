#!/usr/bin/env python3
"""Run native substrate tests and require verified FEX guest dispatch/resume."""
import argparse
import json
import re
import struct
import hashlib
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = Path('/Users/yount/Documents/Projects/raven-components/fex-modules')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', type=Path, default=DEFAULT_ROOT)
    parser.add_argument('--substrate-only', action='store_true')
    parser.add_argument('--host', type=Path)
    parser.add_argument('--abi', type=Path)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix='raven-modhost-test-') as temp:
        source = REPO / 'native/platform/darwin/fex/modhost'
        def run(command, expected=0):
            p = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
            results.append(dict(command=list(map(str, command)), returncode=p.returncode,
                                stdout=p.stdout, stderr=p.stderr))
            print(p.stdout, end='')
            print(p.stderr, end='')
            if p.returncode != expected:
                raise RuntimeError(f'unexpected exit {p.returncode} (expected {expected})')
        try:
            binaries = {}
            for name, supplied, cpp, extra in [
                ('host', args.host, 'native_host.cpp', [source / 'context.S']),
                ('abi', args.abi, 'abi_test.cpp', [source / 'abi_fixture.S']),
            ]:
                binary = supplied or Path(temp) / name
                if not supplied:
                    run(['xcrun', 'clang++', '-arch', 'arm64', '-ffixed-x18', '-std=c++17',
                         '-O2', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror', '-mmacosx-version-min=11.0',
                         '-Wl,-platform_version,macos,11.0,11.0', source / cpp,
                         source / 'abi.S', *extra, '-o', binary])
                binaries[name] = binary
            run([binaries['host'], '--self-test'])
            run([binaries['abi']])
            if not args.substrate_only:
                # Assemble x86 code as data only. Never launch an x86 process.
                obj = Path(temp) / 'kernels.o'
                run(['xcrun', 'clang', '-arch', 'x86_64', '-c', source / 'bench_kernels_x86_64.S', '-o', obj])
                data = obj.read_bytes()
                if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
                    raise RuntimeError('not a Mach-O 64-bit kernel object')
                at = 32
                text = None
                for _ in range(struct.unpack_from('<I', data, 16)[0]):
                    cmd, size = struct.unpack_from('<II', data, at)
                    if cmd == 0x19:
                        for i in range(struct.unpack_from('<I', data, at + 64)[0]):
                            section = at + 72 + i * 80
                            if data[section:section + 16].rstrip(b'\0') == b'__text':
                                length, offset = struct.unpack_from('<QI', data, section + 40)
                                text = data[offset:offset + length]
                    at += size
                header = (source / 'bench.h').read_text()
                expected = []
                for name in ['integer', 'floating']:
                    values = re.search(r'constexpr uint8_t ' + name + r'\[\]\{([^}]+)', header).group(1)
                    expected.append(bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]+)', values)))
                floating_offset = (len(expected[0]) + 15) & ~15
                if text is None or text[:len(expected[0])] != expected[0] or text[floating_offset:] != expected[1]:
                    raise RuntimeError('benchmark bytes differ from original SPIKE assembly')
                print('PASS: benchmark function bodies match original SPIKE assembly bytes')
                module = args.modules / 'xtajit64.dll'
                results.append(dict(module=str(module), module_sha256=hashlib.sha256(module.read_bytes()).hexdigest()))
                run([binaries['host'], '--execute', args.modules / 'xtajit64.dll'])
                output = results[-1]['stdout']
                for required in ['resolved=127', 'PASS: RavenProcessInit', 'PASS: ThreadInit',
                                 'host_result=224 resumed_result=4884 calls=1',
                                 'PASS: precise AV', 'repaired_result=4665',
                                 'outer_resumed_result=5008', 'BENCH sample=4', 'ROUNDTRIP sample=4']:
                    if required not in output:
                        raise RuntimeError('missing execution evidence: ' + required)
        finally:
            if args.evidence:
                args.evidence.write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: substrate only; no module init or guest execution claimed' if args.substrate_only
          else 'PASS: native module init and verified x86-64 dispatch/resume')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        raise SystemExit(str(error))
