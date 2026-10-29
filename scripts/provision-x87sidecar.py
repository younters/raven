#!/usr/bin/env python3
"""Install the pinned, non-entitled cooperative x87sidecar outside the checkout."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

VERSION = "1.9.0"
SHA256 = "104098b0ba8baed37d4d63cf62ab3b55493889b8fe9ee225cad8febcd6bc1e78"
DEFAULT = Path.home() / "Documents/Projects/raven-components/x87sidecar"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT)
    args = parser.parse_args()
    destination = args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    if destination == repo or repo in destination.parents:
        parser.error("the sidecar must be installed outside the repository")
    with tempfile.TemporaryDirectory(prefix="raven-x87-") as work:
        archive = Path(work) / "x87sidecar.tar.xz"
        url = f"https://github.com/athei/x87sidecar/releases/download/v{VERSION}/x87sidecar.tar.xz"
        with urllib.request.urlopen(url, timeout=60) as source, archive.open("wb") as output:
            shutil.copyfileobj(source, output)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError("x87sidecar archive SHA256 mismatch")
        binary = Path(work) / "x87sidecar"
        with tarfile.open(archive, "r:xz") as package:
            member = package.getmember("x87sidecar")
            if not member.isfile() or member.size > 10 * 1024 * 1024:
                raise RuntimeError("invalid flat x87sidecar archive member")
            with package.extractfile(member) as source, binary.open("wb") as output:
                shutil.copyfileobj(source, output)
        binary.chmod(0o755)
        attributes = subprocess.check_output(["/usr/bin/xattr", str(binary)], text=True).splitlines()
        if "com.apple.quarantine" in attributes:
            subprocess.run(["/usr/bin/xattr", "-d", "com.apple.quarantine", str(binary)], check=True)
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, destination / "x87sidecar")
    print(f"Installed flat x87sidecar v{VERSION}: {destination / 'x87sidecar'}")
    print(f"RAVEN_X87_SIDECAR={destination / 'x87sidecar'}")


if __name__ == "__main__":
    main()
