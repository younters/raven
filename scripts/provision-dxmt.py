#!/usr/bin/env python3
"""Provision the pinned DXMT PE/unix pair outside the checkout, replacing its Wine dylib dependencies."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


# The source equivalent is native/platform/darwin/dxmt/patches/0001-allow-foreign-hwnd-swapchain.patch.
# v0.80's PE release is patched here because reproducing that binary needs DXMT's
# x86_64 LLVM 15 and Wine build toolchains. Only the one conditional branch is
# replaced; both complete-file hashes and the surrounding instruction bytes are pinned.
DXMT_D3D11_ORIGINAL = "7ca382af0eb32d8a432f6efb14d594fefb45673663be1f7e6682254bff885c47"
DXMT_D3D11_PATCHED = "2eb017f782fbd6c581a04f74b5f2d5b4d0818d743d997791b1a23521f1cfd587"
DXMT_SWAPCHAIN_OFFSET = 107771
DXMT_SWAPCHAIN_BEFORE = bytes.fromhex("39 44 24 44 0f 85 e3 00 00 00 49 8b 04 24")
DXMT_SWAPCHAIN_AFTER = DXMT_SWAPCHAIN_BEFORE[:4] + b"\x90" * 6 + DXMT_SWAPCHAIN_BEFORE[10:]


def patch_cross_process_swapchain(path):
    data = bytearray(path.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest == DXMT_D3D11_PATCHED:
        return
    if digest != DXMT_D3D11_ORIGINAL or data[DXMT_SWAPCHAIN_OFFSET:DXMT_SWAPCHAIN_OFFSET + 14] != DXMT_SWAPCHAIN_BEFORE:
        raise RuntimeError("DXMT d3d11.dll differs from the pinned v0.80 swap-chain patch input")
    data[DXMT_SWAPCHAIN_OFFSET:DXMT_SWAPCHAIN_OFFSET + 14] = DXMT_SWAPCHAIN_AFTER
    if hashlib.sha256(data).hexdigest() != DXMT_D3D11_PATCHED:
        raise RuntimeError("DXMT swap-chain patch output differs from the pin")
    path.write_bytes(data)


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
    pe_d3d11 = destination / "dxmt/pe64/d3d11.dll"
    if pe_d3d11.exists() and hashlib.sha256(pe_d3d11.read_bytes()).hexdigest() == DXMT_D3D11_PATCHED:
        pe_d3d11.unlink()  # Let the verified archive restore the release file before patching it again.
    graphics.provision(destination, {"dxmt": package})
    patch_cross_process_swapchain(pe_d3d11)
    records = [item for item in previous if item.get("component") != "dxmt"] + json.loads(manifest.read_text())
    for record in records:
        if record.get("component") == "dxmt" and record.get("path") == "dxmt/pe64/d3d11.dll":
            record["sha256"] = DXMT_D3D11_PATCHED
    manifest.write_text(json.dumps(records, indent=2) + "\n")
    root = destination / "dxmt"
    native = root / "unix64"
    native.mkdir(parents=True, exist_ok=True)
    source = repository / "native/platform/darwin/dxmt/unix_adapter.m"
    adapter = native / "raven-dxmt.dylib"
    subprocess.run(["/usr/bin/clang", "-arch", "x86_64", "-dynamiclib", "-O2", "-Wall", "-Wextra", "-fblocks",
                    str(source), str(source.with_name("presentation.m")), "-framework", "Foundation", "-framework", "Metal",
                    "-framework", "IOSurface",
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
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not compiler:
        raise RuntimeError("DXMT's Raven D3D10 entry points need mingw-w64 (brew install mingw-w64)")
    for name in ("d3d10", "d3d10_1"):
        subprocess.run([compiler, "-shared", "-nostdlib", "-O2", "-Wall", "-Wextra", "-Wl,--entry,0",
                        *(["-DRAVEN_D3D10_1=1"] if name == "d3d10_1" else []),
                        str(source.with_name("d3d10.c")), str(source.with_name(f"{name}.def")),
                        "-o", str(root / "pe64" / f"{name}.dll"), f"-Wl,--out-implib,{root / 'pe64' / ('lib' + name + '.a')}",
                        "-ldxgi", "-ldxguid", "-lkernel32", "-lgcc"], check=True)
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
