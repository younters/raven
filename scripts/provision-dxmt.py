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


def provision(destination, source_llvm=None, source_wine=None):
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
    source_manifest = destination / "dxmt/installed.json"
    if source_manifest.exists():
        installed_source = json.loads(source_manifest.read_text())
        if installed_source.get("version") == "source-experimental":
            # Restore the checksum-pinned release before rebuilding. Do not silently
            # discard locally modified source outputs in a reused destination.
            outputs = [destination / "dxmt/pe64" / name for name in
                       ("d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll")]
            for path in outputs:
                expected = installed_source["files"].get(str(path.relative_to(destination / "dxmt")))
                if path.exists() and graphics.digest(path) != expected:
                    raise RuntimeError(f"source DXMT output was modified: {path}")
            for path in outputs:
                path.unlink(missing_ok=True)
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
    (native / "unix-call-count").write_text("132\n")
    source_build = None
    if source_llvm is not None:
        spec = importlib.util.spec_from_file_location("dxmt_source", source.with_name("build_source.py"))
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        source_build = builder.build(root, source_llvm, source_wine)
        for record in records:
            if record.get("component") == "dxmt":
                path = destination / record["path"]
                if path.is_file():
                    record["sha256"] = graphics.digest(path)
        manifest.write_text(json.dumps(records, indent=2) + "\n")
    provenance = repository / "native/upstream/dxmt"
    shutil.copytree(provenance, root / "licenses", dirs_exist_ok=True)
    if source_build:
        source_root = destination / "dxmt-source-build/src"
        for name in ("LICENSE", "LICENSE.OLD", "COPYING.LIB"):
            shutil.copy2(source_root / name, root / "licenses" / ("DXMT." + name))
        (root / "licenses/SOURCE.md").write_text(
            "Source overlay: https://github.com/3Shain/dxmt/commit/" + source_build["commit"] + "\n"
            "The source pin uses LGPL-2.1-or-later; DXMT.LICENSE and DXMT.COPYING.LIB are copied from that pin.\n"
            "README.md describes only the fallback release. The provisioner-owned source checkout and patches\n"
            "are retained alongside this overlay in ../dxmt-source-build/src.\n")
    installed = {"version": package["version"], "commit": package["commit"], "archive_sha256": package["sha256"],
                 "files": {str(path.relative_to(root)): graphics.digest(path)
                           for path in sorted(root.rglob("*")) if path.is_file() and path.name != "installed.json"}}
    if source_build:
        installed.update(version="source-experimental", commit=source_build["commit"], source_build=source_build)
    (root / "installed.json").write_text(json.dumps(installed, indent=2) + "\n")
    print(f"DXMT unixlib and Raven adapter verified; RAVEN_DXMT_ROOT={root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True, help="external graphics cache; creates dxmt/ inside it")
    parser.add_argument("--source-llvm", type=Path, help="experimental source build: x86_64 LLVM 15 installation")
    parser.add_argument("--source-wine", type=Path, help="configured Wine build with winebuild and x86_64 import libraries")
    arguments = parser.parse_args()
    if bool(arguments.source_llvm) != bool(arguments.source_wine):
        parser.error("--source-llvm and --source-wine must be supplied together")
    try:
        provision(arguments.destination.resolve(), arguments.source_llvm.resolve() if arguments.source_llvm else None,
                  arguments.source_wine.resolve() if arguments.source_wine else None)
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"DXMT provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
