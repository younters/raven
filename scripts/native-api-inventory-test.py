#!/usr/bin/env python3
"""Ensure explicitly shared x86 exports have live registered handlers."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


def main():
    raven, program, source = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="raven-api-inventory-") as work:
        snapshot = Path(work) / "x86.tsv"
        result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=30000", str(program)],
                                env=os.environ | {"RAVEN_PREFIX": str(Path(work) / "prefix"), "RAVEN_API_INVENTORY": str(snapshot)},
                                capture_output=True, text=True, timeout=35)
        if result.returncode or "Native entry returned 0 " not in result.stdout:
            raise RuntimeError(result.stdout + result.stderr)
        hosts = {line.split("\t")[1] for line in snapshot.read_text().splitlines() if line.startswith("host\t")}
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", source.read_text(), flags=re.DOTALL)

        def initializer(name):
            pattern = rf"\b{name}\s*(?:\[\s*\])?\s*=?\s*\{{(.*?)\}}\s*;"
            match = re.search(pattern, text, re.DOTALL)
            if not match:
                raise RuntimeError(f"x86 API inventory: missing {name} initializer")
            return match.group(1)

        names = set(re.findall(r'"(\w+)"', initializer("shared_allowed")))
        names.update(re.findall(r'Export\s*\{\s*"[^"]+"\s*,\s*"(\w+)"', initializer("shared_exports")))
        names.update(re.findall(r'\{\s*"(\w+)"', initializer("wide_arguments")))
        missing = sorted(names - hosts)
        if missing:
            raise RuntimeError("x86 shared export list has no registered handler: " + ", ".join(missing))
        print(f"All {len(names)} explicitly shared x86 APIs have registered handlers")


if __name__ == "__main__":
    main()
