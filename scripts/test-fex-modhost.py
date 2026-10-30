#!/usr/bin/env python3
"""Run native substrate tests and require verified FEX guest dispatch/resume."""
import argparse
import json
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
                         '-O2', '-Wall', '-Wextra', '-Werror', '-mmacosx-version-min=11.0',
                         '-Wl,-platform_version,macos,11.0,11.0', source / cpp,
                         source / 'abi.S', *extra, '-o', binary])
                binaries[name] = binary
            run([binaries['host'], '--self-test'])
            run([binaries['abi']])
            if not args.substrate_only:
                run([binaries['host'], '--execute', args.modules / 'xtajit64.dll'])
                output = results[-1]['stdout']
                for required in ['resolved=127', 'PASS: RavenProcessInit', 'PASS: ThreadInit',
                                 'host_result=224 resumed_result=4884 calls=1']:
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
