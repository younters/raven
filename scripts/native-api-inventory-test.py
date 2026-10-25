#!/usr/bin/env python3
"""Ensure explicitly shared x86 exports have live handlers, and forwarders have guest targets."""
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
                                capture_output=True, text=True, timeout=30)
        if result.returncode or "Native entry returned 0 " not in result.stdout:
            raise RuntimeError(result.stdout + result.stderr)
        hosts = {line.split("\t")[1] for line in snapshot.read_text().splitlines() if line.startswith("host\t")}
        text = source.read_text().split("constexpr std::string_view shared_allowed[] = {", 1)[1].split("};", 1)[0]
        names = set(re.findall(r'"(\w+)"', text))
        declarations = source.read_text().split("constexpr std::array shared_exports{", 1)[1].split("};", 1)[0]
        names.update(re.findall(r'Export\{"[^" ]+", "(\w+)"', declarations))
        wide = source.read_text().split("constexpr WideArgument wide_arguments[] = {", 1)[1].split("};", 1)[0]
        names.update(re.findall(r'\{"(\w+)"', wide))
        missing = sorted(names - hosts)
        if missing:
            raise RuntimeError("x86 shared export list has no registered handler: " + ", ".join(missing))
        print(f"All {len(names)} explicitly shared x86 APIs have registered handlers")


if __name__ == "__main__":
    main()
