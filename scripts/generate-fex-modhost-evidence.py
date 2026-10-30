#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate M1d evidence from a Release -j8 build, locked CTest and live guest run."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from fex_modhost_contract import REPO, sha, verify_module_pin, verify_patch_pins

SOURCE = REPO / 'native/platform/darwin/fex/modhost'


def samples(output, prefix='BENCH '):
    found = re.findall(re.escape(prefix) + r'sample=(\d+) (?:iterations=10000000 )?integer_ns=(\d+) float_ns=(\d+)', output)
    if [int(row[0]) for row in found] != list(range(5)):
        raise RuntimeError('expected exactly five ordered verified compute samples')
    return [dict(integer_ns=int(i), float_ns=int(f)) for _, i, f in found]


def medians(rows):
    return {name + '_ms': statistics.median(row[name + '_ns'] for row in rows) / 1e6
            for name in ('integer', 'float')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', type=Path, default=os.environ.get('RAVEN_FEX_MODULE_ROOT'))
    parser.add_argument('--output', type=Path, default=SOURCE / 'm1d-runtime-evidence.json')
    args = parser.parse_args()
    if args.modules is None:
        parser.error('set RAVEN_FEX_MODULE_ROOT or pass --modules')
    root = args.modules.resolve()
    pins = verify_patch_pins()
    verify_module_pin(root / 'xtajit64.dll', pins)
    manifest = json.loads((root / 'm1d-manifest.json').read_text())
    if manifest['build']['status'] != 'pass':
        raise RuntimeError('module build audit did not pass')
    for name, item in manifest['build']['artifacts'].items():
        if sha(root / name) != item['sha256']:
            raise RuntimeError('manifest artifact identity mismatch: ' + name)
    for item in manifest['build']['licenses']:
        if sha(root / 'licenses' / item['path'].replace('/', '__')) != item['sha256']:
            raise RuntimeError('missing or changed license: ' + item['path'])
    records = []

    def run(command):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=180)
        records.append(dict(command=list(map(str, command)), returncode=result.returncode,
                            stdout=result.stdout, stderr=result.stderr))
        print(result.stdout, end='', flush=True)
        print(result.stderr, end='', flush=True)
        if result.returncode:
            raise RuntimeError('evidence command failed: ' + str(command))
        return result.stdout

    with tempfile.TemporaryDirectory(prefix='raven-fex-evidence-') as temp:
        build = Path(temp) / 'build'
        run(['cmake', '-S', SOURCE, '-B', build, '-DRAVEN_FEX_MODHOST=ON',
             '-DCMAKE_BUILD_TYPE=Release', '-DRAVEN_FEX_MODULE_ROOT=' + str(root)])
        run(['cmake', '--build', build, '-j8'])
        junit = Path(temp) / 'ctest.xml'
        run(['/tmp/raven-ctest-lock.sh', 'fex-review-evidence', 'ctest', '--test-dir', build,
             '--output-on-failure', '--output-junit', junit])
        suite = ET.parse(junit).getroot()
        cases = suite.findall('.//testcase')
        expected = {'fex_modhost_substrate', 'fex_modhost_execution',
                    'fex_modhost_tier0_gap_expected_failure', 'fex_modhost_driver_contract'}
        if {case.attrib['name'] for case in cases} != expected or any(
                case.find(tag) is not None for case in cases for tag in ('failure', 'error', 'skipped')):
            raise RuntimeError('provisioned CTest suite was incomplete, failed or skipped')
        raw = Path(temp) / 'runtime.json'
        run([sys.executable, REPO / 'scripts/test-fex-modhost.py', '--modules', root,
             '--host', build / 'fex_modhost_substrate', '--abi', build / 'fex_modhost_abi', '--evidence', raw])
        runtime = json.loads(raw.read_text())
    executed = [r for r in runtime if '--execute' in r.get('command', [])]
    if len(executed) != 1 or executed[0]['returncode']:
        raise RuntimeError('expected exactly one passing native module execution')
    output = executed[0]['stdout']
    current = samples(output)
    baseline_path = SOURCE / 'baseline/rosetta-bench.txt'
    baseline = samples(baseline_path.read_text(), '')
    full, warm = medians(current), medians(current[1:])
    prior, prior_warm = medians(baseline), medians(baseline[1:])
    trips = re.findall(r'ROUNDTRIP sample=(\d+) calls=100000 elapsed_ns=(\d+) ', output)
    if [int(i) for i, _ in trips] != list(range(5)):
        raise RuntimeError('expected exactly five ordered round-trip samples')
    trips = [int(n) / 100000 for _, n in trips]
    evidence = dict(status='execution-and-review-regressions-pass',
                    source_commit=manifest['source_commit'], build_id=manifest['build_id'],
                    module_build_cached=manifest['cached'], pins=pins,
                    artifacts={name: dict(sha256=sha(root / name)) for name in manifest['build']['artifacts']},
                    licenses=manifest['build']['licenses'], host_pages=os.sysconf('SC_PAGE_SIZE'), entitlement=False,
                    ctest=dict(total=len(cases), passed=len(cases), transcript=records[:-1]),
                    samples=current, medians=full, warm_medians_samples_1_to_4=warm,
                    prior_rosetta_ms=prior, prior_rosetta_warm_ms=prior_warm,
                    fex_over_rosetta={name: full[name + '_ms'] / prior[name + '_ms'] for name in ('integer', 'float')},
                    warm_fex_over_full_rosetta={name: warm[name + '_ms'] / prior[name + '_ms'] for name in ('integer', 'float')},
                    roundtrip_samples_ns=trips, roundtrip_median_ns=statistics.median(trips),
                    roundtrip_warm_median_ns=statistics.median(trips[1:]),
                    baseline_sha256={p.name: sha(p) for p in sorted((SOURCE / 'baseline').iterdir())},
                    host_source_sha256={str(p.relative_to(REPO)): sha(p) for p in sorted(SOURCE.iterdir())
                                        if p.suffix in ('.h', '.c', '.cpp', '.S')},
                    script_sha256={name: sha(REPO / 'scripts' / name) for name in
                                   ('test-fex-modhost.py', 'generate-fex-modhost-evidence.py', 'fex_modhost_contract.py')},
                    runtime=runtime, runtime_command=records[-1])
    args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print('PASS: generated evidence from checked live execution: ' + str(args.output))


if __name__ == '__main__':
    main()
