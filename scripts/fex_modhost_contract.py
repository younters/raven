# SPDX-License-Identifier: MIT
"""Version-controlled input contract for the standalone FEX qualification driver."""
import hashlib
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_patch_pins(repo=REPO):
    pins = json.loads((repo / 'scripts/fex-modhost-pins.json').read_text())
    directory = repo / 'native/platform/darwin/fex/patches'
    actual = {p.name: sha(p) for p in directory.glob('*.patch')}
    if actual != pins['patches']:
        raise RuntimeError('FEX patch sha256/set differs from pinned qualification inputs')
    return pins


def verify_module_pin(module, pins):
    actual = sha(module)
    if actual != pins['xtajit64_sha256']:
        raise RuntimeError(f'xtajit64.dll sha256 mismatch: expected {pins["xtajit64_sha256"]}, got {actual}')
    return actual
