#!/usr/bin/env python3
"""Portable root-selection regressions. No module execution is faked."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).with_name('test-fex-process.py').resolve()
class Roots(unittest.TestCase):
    def test_empty_root_is_unset_even_with_dll_in_cwd(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, 'xtajit64.dll').write_bytes(b'not a module')
            for arguments, environment in [([], {}), (['--modules', ''], {'RAVEN_FEX_MODULE_ROOT': ''})]:
                env = dict(os.environ)
                env.pop('RAVEN_FEX_MODULE_ROOT', None)
                env.update(environment)
                result = subprocess.run([sys.executable, str(RUNNER), '--raven', '/unused', '--fixture', '/unused', *arguments],
                                        cwd=directory, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 77, result.stderr)
                self.assertIn('SKIP: provision', result.stdout)
if __name__ == '__main__':
    unittest.main()
