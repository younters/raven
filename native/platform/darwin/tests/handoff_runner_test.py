"""A printed measurement must never mask benchmark failure."""
import pathlib
import subprocess
import sys
import unittest


class HandoffRunnerTest(unittest.TestCase):
    def run_case(self, output, status):
        runner = pathlib.Path(__file__).with_name('run_handoff.py')
        return subprocess.run(
            [sys.executable, str(runner), sys.executable, '-c',
             f'print({output!r}); raise SystemExit({status})'],
            capture_output=True, timeout=5).returncode

    def test_success(self):
        self.assertEqual(self.run_case('2000 100 150', 0), 0)

    def test_print_then_fail(self):
        self.assertNotEqual(self.run_case('2000 100 150', 9), 0)

    def test_missing_measurements(self):
        self.assertNotEqual(self.run_case('', 0), 0)


if __name__ == '__main__':
    unittest.main()
