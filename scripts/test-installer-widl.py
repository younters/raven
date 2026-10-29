#!/usr/bin/env python3
"""Check the actual WIDL client calling convention without rebuilding an installer."""
import os
import shutil
import sys
from pathlib import Path
import subprocess
import tempfile

if not shutil.which("widl"):
    print("WIDL unavailable")
    sys.exit(77)
with tempfile.TemporaryDirectory(prefix="raven-widl-") as directory:
    root = Path(directory)
    for triple, platform, stack in (("i686-w64-mingw32", "WIN32", True), ("x86_64-w64-mingw32", "WIN64", False)):
        output = root / triple / "probe.c"
        subprocess.run([sys.executable, str(Path(__file__).with_name("provision-installer.py")),
                        "--arch", "x86" if stack else "x64", "--output", str(output.parent), "--rpc-abi-probe"], check=True)
        text = output.read_text()
        assert "__RPC_" + platform + "__" in text, triple
        call = text.split("NdrClientCall2(", 1)[1].split(";", 1)[0]
        assert ("&__params" in call) == stack, call
        print(triple + ": correct RPC client ABI")
