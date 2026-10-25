#!/usr/bin/env python3
"""Run an isolated hardware DXMT fixture and validate its output."""
from pathlib import Path
import os
import shutil
import subprocess
import sys


def main():
    raven, workload, root, work = (Path(value).resolve() for value in sys.argv[1:5])
    kind = sys.argv[5]
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    prefix = work / "prefix"
    env = os.environ | {"RAVEN_DXMT_ROOT": str(root), "RAVEN_PREFIX": str(prefix)}
    env.pop("RAVEN_D3D_DIRECTORY", None)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=90000", str(workload)],
                            capture_output=True, text=True, timeout=110, env=env)
    (work / "stdout.log").write_text(result.stdout)
    (work / "stderr.log").write_text(result.stderr)
    status = prefix / "drive_c" / ("dxmt-device.txt" if kind == "device" else "triangle.txt")
    text = status.read_text() if status.exists() else "missing"
    if result.returncode or not text.startswith("OK"):
        raise SystemExit(f"DXMT {kind}: exit {result.returncode}, {text!r}\n{result.stderr[-3000:]}")
    if kind == "triangle":
        checker = Path(__file__).resolve().parents[4] / "scripts/check-d3d11-triangle.py"
        subprocess.run([sys.executable, str(checker), str(prefix / "drive_c/triangle.raw")], check=True)
    else:
        print(text)


if __name__ == "__main__":
    main()
