#!/usr/bin/env python3
"""Provision the pinned DXMT PE/unix pair outside the checkout, replacing its Wine dylib dependencies."""

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


def provision(destination):
    if sys.platform != "darwin":
        raise RuntimeError("the DXMT adapter must be built on macOS")
    repository = Path(__file__).resolve().parent.parent
    if destination == repository or repository in destination.parents:
        raise RuntimeError("DXMT binaries must be provisioned outside the checkout")
    spec = importlib.util.spec_from_file_location("raven_graphics", repository / "scripts/provision-graphics.py")
    graphics = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(graphics)
    package = json.loads((repository / "native/graphics/backends.json").read_text())["dxmt"]
    # Reuse the bounded, checksum-verified downloader and PE allowlist extraction.
    manifest = destination / "installed.json"
    previous = json.loads(manifest.read_text()) if manifest.exists() else []
    graphics.provision(destination, {"dxmt": package})
    records = [item for item in previous if item.get("component") != "dxmt"] + json.loads(manifest.read_text())
    manifest.write_text(json.dumps(records, indent=2) + "\n")
    root = destination / "dxmt"
    native = root / "unix64"
    native.mkdir(parents=True, exist_ok=True)
    source = repository / "native/platform/darwin/dxmt/unix_adapter.m"
    adapter = native / "raven-dxmt.dylib"
    subprocess.run(["/usr/bin/clang", "-arch", "x86_64", "-dynamiclib", "-O2", "-Wall", "-Wextra", "-fblocks",
                    str(source), "-framework", "Foundation", "-framework", "Metal",
                    "-Wl,-install_name,@rpath/raven-dxmt.dylib", "-o", str(adapter)], check=True)
    with tarfile.open(destination / package["archive"], "r:gz") as archive:
        entry = archive.getmember(f'{package["archive_root"]}/x86_64-unix/winemetal.so')
        if not entry.isfile() or entry.size > 100 * 1024 * 1024:
            raise RuntimeError("invalid DXMT unixlib archive entry")
        with archive.extractfile(entry) as content, (native / "winemetal.so").open("wb") as output:
            shutil.copyfileobj(content, output)
    library = native / "winemetal.so"
    subprocess.run(["/usr/bin/install_name_tool", "-change", "@rpath/winemac.so", "@loader_path/raven-dxmt.dylib",
                    "-change", "@rpath/ntdll.so", "@loader_path/raven-dxmt.dylib", str(library)], check=True)
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(library)], check=True)
    provenance = repository / "native/upstream/dxmt"
    shutil.copytree(provenance, root / "licenses", dirs_exist_ok=True)
    installed = {"version": package["version"], "commit": package["commit"], "archive_sha256": package["sha256"],
                 "files": {str(path.relative_to(root)): graphics.digest(path)
                           for path in sorted(root.rglob("*")) if path.is_file() and path.name != "installed.json"}}
    (root / "installed.json").write_text(json.dumps(installed, indent=2) + "\n")
    print(f"DXMT unixlib and Raven adapter verified; RAVEN_DXMT_ROOT={root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True, help="external graphics cache; creates dxmt/ inside it")
    arguments = parser.parse_args()
    try:
        provision(arguments.destination.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"DXMT provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
