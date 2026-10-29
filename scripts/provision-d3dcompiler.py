#!/usr/bin/env python3
"""Install Microsoft's SDK redistributable HLSL compiler into an external DXMT overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request

URL = ('https://download.microsoft.com/download/38ab8f6d-3676-4860-ae84-3361308b9d7f/'
       'KIT_BUNDLE_WINDOWSSDK_MEDIACREATION/Installers/61d57a7a82309cd161a854a6f4619e52.cab')
SHA256 = 'd951f1eefc051643ed28a0ba8bee804097bb9d6e7ee309d5c1be04f8272c0486'
MEMBER = 'fil3585cb2ea5db13cc0838f8d06b5c9679'
MAX_SIZE = 16 * 1024 * 1024


def provision(destination):
    destination = Path(destination).resolve()
    repository = Path(__file__).resolve().parent.parent
    if destination == repository or repository in destination.parents:
        raise RuntimeError('Microsoft binaries must stay outside the checkout')
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='raven-hlsl-') as scratch:
        archive = Path(scratch) / 'compiler.cab'
        with urllib.request.urlopen(URL, timeout=45) as response, archive.open('wb') as output:
            received = 0
            while chunk := response.read(1024 * 1024):
                received += len(chunk)
                if received > MAX_SIZE:
                    raise RuntimeError('compiler archive exceeds the pinned size limit')
                output.write(chunk)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError('Microsoft compiler archive checksum mismatch')
        # libarchive on macOS reads CAB files; select one member, without extracting paths.
        data = subprocess.run(['/usr/bin/tar', '-xOf', str(archive), MEMBER],
                              check=True, capture_output=True).stdout
        if not data or len(data) > 8 * 1024 * 1024:
            raise RuntimeError('invalid compiler archive member size')
        target = destination / 'd3dcompiler_47.dll'
        temporary = destination / '.d3dcompiler_47.dll.partial'
        temporary.write_bytes(data)
        temporary.replace(target)
    record = {'provider': 'Microsoft Windows SDK redistributable', 'architecture': 'x86_64',
              'url': URL, 'archive_sha256': SHA256,
              'sha256': hashlib.sha256(data).hexdigest()}
    (destination / 'd3dcompiler_47.provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Microsoft HLSL compiler installed: {target}')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True, help='external directory for d3dcompiler_47.dll')
    args = parser.parse_args()
    try:
        provision(args.destination)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'compiler provisioning failed: {error}\n')
