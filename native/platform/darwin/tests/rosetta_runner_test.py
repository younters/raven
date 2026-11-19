import subprocess
import unittest
from unittest.mock import patch
import run_rosetta


class RunnerTest(unittest.TestCase):
    @patch("run_rosetta.subprocess.call")
    @patch("run_rosetta.subprocess.run", return_value=subprocess.CompletedProcess([], 1))
    def test_missing_rosetta(self, probe, execute):
        self.assertEqual(run_rosetta.main(["fixture"]), 77)
        execute.assert_not_called()

    @patch("run_rosetta.subprocess.call", return_value=3)
    @patch("run_rosetta.subprocess.run", return_value=subprocess.CompletedProcess([], 0))
    def test_preserves_fixture_failure(self, probe, execute):
        self.assertEqual(run_rosetta.main(["fixture"]), 3)
        execute.assert_called_once_with(["fixture"])


if __name__ == "__main__":
    unittest.main()
