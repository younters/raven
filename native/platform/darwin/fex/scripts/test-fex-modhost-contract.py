#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Portable negative tests for the FEX module and patch pins and the test driver's contract."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from fex_modhost_contract import REPO, verify_module_pin, verify_patch_pins

SCRIPTS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('driver', SCRIPTS / 'test-fex-modhost.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class Contract(unittest.TestCase):
    def test_expected_failure_does_not_hide_errors(self):
        argv = ['--tier0-gap', '--host', '/unused', '--abi', '/unused']
        expected = 'XFAIL: first_faults=1 second_faults=0'
        for rc, output, code in [(1, expected, 1), (0, expected, 0), (-6, expected, 0), (1, 'unrelated failure', 0)]:
            with self.subTest(rc=rc, output=output), patch.object(driver.subprocess, 'run', return_value=
                    subprocess.CompletedProcess([], rc, output, '')):
                self.assertEqual(driver.entry(argv), code)
        for error in (OSError('launch failed'), subprocess.TimeoutExpired('probe', 10)):
            with self.subTest(error=error), patch.object(driver.subprocess, 'run', side_effect=error):
                self.assertEqual(driver.entry(argv), 0)

    def test_actual_patches_and_rva(self):
        pins = verify_patch_pins()
        header = (REPO / 'native/platform/darwin/fex/modhost/module_contract.h').read_text()
        self.assertIn(f'0x{pins["raven_process_init_rva"]:x}', header)
        self.assertEqual(pins['raven_wow64_process_init_rva'], 0x79cc)
        self.assertEqual(pins['raven_wow64_process_init_instruction'], 0xf81f0ffe)
        self.assertIn(f'wow64_process_init_rva = 0x{pins["raven_wow64_process_init_rva"]:x}', header)
        self.assertIn(f'wow64_process_init_instruction = 0x{pins["raven_wow64_process_init_instruction"]:x}', header)

    def test_module_rejects_changed_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            module = Path(temp) / 'xtajit64.dll'
            module.write_bytes(b'fixture')
            pins = dict(xtajit64_sha256=hashlib.sha256(b'fixture').hexdigest())
            verify_module_pin(module, pins)
            module.write_bytes(b'changed')
            with self.assertRaisesRegex(RuntimeError, 'sha256 mismatch'):
                verify_module_pin(module, pins)

    def test_patch_rejects_modified_missing_extra(self):
        for mutation in ('modified', 'missing', 'extra'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                patches = root / 'native/platform/darwin/fex/patches'
                patches.mkdir(parents=True)
                scripts = patches.with_name('scripts')
                scripts.mkdir()
                patch = patches / 'fixture.patch'
                patch.write_bytes(b'fixture')
                (scripts / 'fex-modhost-pins.json').write_text(json.dumps(dict(
                    patches={patch.name: hashlib.sha256(b'fixture').hexdigest()})))
                verify_patch_pins(root)
                if mutation == 'modified':
                    patch.write_bytes(b'changed')
                elif mutation == 'missing':
                    patch.unlink()
                else:
                    (patches / 'extra.patch').write_bytes(b'extra')
                with self.assertRaisesRegex(RuntimeError, 'patch sha256/set'):
                    verify_patch_pins(root)

    def test_missing_module_is_visible_skip(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([sys.executable, SCRIPTS / 'test-fex-modhost.py', '--modules', temp],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 77)
            self.assertIn('SKIP: provision xtajit64.dll', result.stdout)

    def test_missing_wow64_module_is_visible_skip(self):
        # The WoW64 tests are registered only when the module is there (wow64_tests.cmake); one that goes missing later reports a skip, which
        # CTest counts as a failure because those tests do not declare SKIP_RETURN_CODE.
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([sys.executable, SCRIPTS / 'test-fex-wow64.py',
                                    '--modules', temp, '--raven', '/unused', '--fixture', '/unused'],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 77)
            self.assertIn('SKIP: provision', result.stdout)

    def test_build_requires_explicit_root(self):
        env = dict(os.environ)
        env.pop('RAVEN_FEX_MODULE_ROOT', None)
        result = subprocess.run([sys.executable, SCRIPTS / 'build-fex-modules.py', '--build-only'],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('set RAVEN_FEX_MODULE_ROOT or pass --root', result.stderr)


if __name__ == '__main__':
    unittest.main()
