#!/usr/bin/env python3
"""Import the pinned arm64 KosmicKrisp ICD from an obtained SDK into a new external root.

Does not download or run an installer, accept agreements, or modify an existing component.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def provision(sdk, destination):
    repository = Path(__file__).resolve().parent.parent
    if destination == repository or repository in destination.parents:
        raise RuntimeError("the ICD must be provisioned outside the checkout")
    if destination == sdk or sdk in destination.parents or destination in sdk.parents:
        raise RuntimeError("source SDK and destination must be separate directories")
    package = json.loads((repository / "native/platform/darwin/graphics/kosmickrisp.json").read_text())
    for name, expected in package["files"].items():
        path = sdk / name
        if not path.is_file() or digest(path) != expected:
            raise RuntimeError(f"missing or unpinned SDK file: {path}; requires SDK {package['sdk_version']}")
    manifest = sdk / "share/vulkan/icd.d/libkosmickrisp_icd.json"
    icd = json.loads(manifest.read_text())["ICD"]
    if icd["library_path"] != "../../../lib/libvulkan_kosmickrisp.dylib":
        raise RuntimeError("unexpected ICD library path")
    if destination.exists():
        # A second invocation can verify our complete output, but never repairs or replaces shared files in place.
        installed = destination / "installed.json"
        if not installed.is_file() or json.loads(installed.read_text()) != package:
            raise RuntimeError("destination exists without the expected manifest; choose a new component root")
        for name, expected in package["files"].items():
            if not (destination / name).is_file() or digest(destination / name) != expected:
                raise RuntimeError(f"installed file differs: {destination / name}; choose a new component root")
        source_notices = repository / "native/upstream/kosmickrisp"
        for name in ("README.md", "MIT.LICENSE"):
            notice = destination / "licenses" / name
            if not notice.is_file() or digest(notice) != digest(source_notices / name):
                raise RuntimeError(f"installed notice differs: {notice}; choose a new component root")
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".raven-kosmickrisp-", dir=destination.parent) as temporary:
            stage = Path(temporary) / "component"
            for name in package["files"]:
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(sdk / name, target)
                if digest(target) != package["files"][name]:
                    raise RuntimeError(f"SDK changed while copying: {name}")
            shutil.copytree(repository / "native/upstream/kosmickrisp", stage / "licenses")
            (stage / "installed.json").write_text(json.dumps(package, indent=2) + "\n")
            stage.rename(destination)
    print(f"Pinned arm64 KosmicKrisp SDK {package['sdk_version']} verified")
    print(f"cmake -S . -B build -DRAVEN_KOSMICKRISP_ROOT={destination}")
    print(f"VK_DRIVER_FILES={destination / 'share/vulkan/icd.d/libkosmickrisp_icd.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="obtained SDK's macOS directory")
    parser.add_argument("--destination", type=Path, required=True, help="new directory outside the checkout")
    arguments = parser.parse_args()
    try:
        provision(arguments.sdk.resolve(), arguments.destination.resolve())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        parser.exit(1, f"KosmicKrisp provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
