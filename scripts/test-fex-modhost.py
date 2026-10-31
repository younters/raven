#!/usr/bin/env python3
"""Run native substrate tests and require verified FEX guest dispatch/resume."""
import argparse
import os
import json
import re
import struct
import hashlib
import signal
import sys
from fex_modhost_contract import verify_patch_pins, verify_module_pin
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = os.environ.get('RAVEN_FEX_MODULE_ROOT')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', type=Path, default=DEFAULT_ROOT)
    parser.add_argument('--tier0-gap', action='store_true', help='Expected-failure probe for CTest WILL_FAIL')
    parser.add_argument('--substrate-only', action='store_true')
    parser.add_argument('--host', type=Path)
    parser.add_argument('--abi', type=Path)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args(argv)
    if not args.substrate_only and not args.tier0_gap and (not args.modules or not (args.modules / 'xtajit64.dll').is_file()):
        print('SKIP: provision xtajit64.dll and set RAVEN_FEX_MODULE_ROOT or --modules')
        return 77
    pins = verify_patch_pins() if not args.substrate_only and not args.tier0_gap else None
    if pins is not None:
        try:
            verify_module_pin(args.modules / "xtajit64.dll", pins)
        except (RuntimeError, OSError) as error:
            parser.error(str(error))
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
            if args.tier0_gap:
                p = subprocess.run([str(binaries['host']), '--tier0-second-fault'], capture_output=True, text=True, timeout=10)
                print(p.stdout, end='');print(p.stderr, end='')
                # WILL_FAIL must invert only this exact known limitation. A crash,
                # unrelated error, or an unexpected pass returns zero and fails CTest.
                return 1 if p.returncode == 1 and 'XFAIL: first_faults=1 second_faults=0' in p.stdout else 0
            run([binaries['host'], '--self-test'])
            run([binaries['abi']])
            run([binaries['host'], '--unsupported-unwind'], expected=-signal.SIGABRT)
            if 'unhandled module NT exception c00000bb' not in results[-1]['stderr']:
                raise RuntimeError('unsupported unwind was not explicitly rejected')
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
                if len(expected[0]) != 27 or len(expected[1]) != 19:
                    raise RuntimeError('SPIKE kernel lengths must be exactly 27 and 19')
                floating_offset = (len(expected[0]) + 15) & ~15
                if text is None or text[:len(expected[0])] != expected[0] or text[floating_offset:] != expected[1]:
                    raise RuntimeError('benchmark bytes differ from original SPIKE assembly')
                print('PASS: benchmark function bodies match original SPIKE assembly bytes')
                module = args.modules / 'xtajit64.dll'
                results.append(dict(module=str(module), module_sha256=hashlib.sha256(module.read_bytes()).hexdigest()))
                run([binaries['host'], '--execute', args.modules / 'xtajit64.dll'])
                output = results[-1]['stdout']
                for required in ['resolved=127', 'auxiliary_iat_writes=127', 'PASS: native PE DllMain attach', 'PASS: RavenProcessInit', 'PASS: ThreadInit',
                                 'host_result=224 resumed_result=4884 calls=1',
                                 'PASS: precise AV', 'repaired_result=4665',
                                 'outer_resumed_result=5008', 'PASS: occupied x87 dispatch and AV', 'BENCH sample=4', 'ROUNDTRIP sample=4']:
                    if required not in output:
                        raise RuntimeError('missing execution evidence: ' + required)
        finally:
            if args.evidence:
                args.evidence.write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: substrate only; no module init or guest execution claimed' if args.substrate_only
          else 'PASS: native module init and verified x86-64 dispatch/resume')


def entry(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        return main(argv)
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print(str(error), file=sys.stderr)
        # CTest WILL_FAIL must never treat a timeout/build/launch error as the
        # known page-granularity failure. Only the exact probe result returns 1.
        return 0 if '--tier0-gap' in argv else 1


if __name__ == '__main__':
    raise SystemExit(entry())
