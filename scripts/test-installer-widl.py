#!/usr/bin/env python3
"""Check the actual WIDL client calling convention without rebuilding an installer."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location("installer", Path(__file__).with_name("provision-installer.py"))
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
with tempfile.TemporaryDirectory(prefix="raven-widl-") as directory:
    root = Path(directory)
    source = root / "probe.idl"
    source.write_text("""[uuid(39c732ad-1d36-47f3-9ca8-a8cc307aa859), version(1.0), explicit_handle]
interface probe {
    long check([in] handle_t binding, [in] long first, [in] long second);
}
""")
    for triple, platform, stack in (("i686-w64-mingw32", "WIN32", True), ("x86_64-w64-mingw32", "WIN64", False)):
        output = root / (triple + ".c")
        subprocess.run(installer.widl_target(triple) + ["-Oif", "-c", "-o", str(output), str(source)], check=True)
        text = output.read_text()
        assert "__RPC_" + platform + "__" in text, triple
        call = text.split("NdrClientCall2(", 1)[1].split(";", 1)[0]
        assert ("&__params" in call) == stack, call
        print(triple + ": correct RPC client ABI")
