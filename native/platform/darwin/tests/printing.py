#!/usr/bin/env python3
"""Private-prefix printer contract; never submits to a physical queue."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

raven, fixture, work = (Path(p).resolve() for p in sys.argv[1:4])
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
env = os.environ | {"RAVEN_PREFIX": str(work / "prefix"), "RAVEN_PRINT_PDF_ONLY": "1"}
p = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=30000", str(fixture)], env=env, capture_output=True, text=True, timeout=45)
(work / "runtime.log").write_text(p.stdout+p.stderr)
if p.returncode:
    raise SystemExit(f"fixture failed ({p.returncode}): {p.stdout}{p.stderr}")
pdf = work / "prefix/drive_c/printing.pdf"
assert pdf.exists(), "EndDoc did not create PDF"
assert not (pdf.parent / "aborted.pdf").exists(), "AbortDoc left a PDF"
assert not list(pdf.parent.glob("*.pdf.*")), "job left staging files"
assert (pdf.parent / "existing.pdf").read_bytes() == b"keep", "AbortDoc overwrote existing output"
subprocess.run(["swift", str(Path(__file__).with_name("printing_pdf.swift")), str(pdf)], check=True, timeout=45)
print(pdf)
