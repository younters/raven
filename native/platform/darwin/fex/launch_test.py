#!/usr/bin/env python3
"""Exercise routing before CLI dispatch without requiring provisioned modules."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='raven-fex-launch-') as directory:
    root = Path(directory)
    raven = root / 'raven'
    # A hard link preserves a private executable-relative deployment directory.
    os.link(sys.argv[1], raven)
    host = root / 'raven-fex-host'
    host.write_text('#!/bin/sh\n[ "$RAVEN_FEX_REEXEC" = 1 ] || exit 72\n[ "$2" = --execution=fex ] || exit 71\nexit 73\n')
    host.chmod(0o755)
    env = dict(os.environ)
    env.pop('RAVEN_FEX_REEXEC', None)
    def run(mode, engine='fex'):
        return subprocess.run([str(raven), mode, '--execution=' + engine, '/nonexistent/fixture.exe'],
                              env=env, capture_output=True, text=True, timeout=10)
    for mode in ['--run-process', '--run-entry', '--run-modules']:
        assert run(mode).returncode == 73, mode
        assert run(mode, 'native').returncode != 73, mode
    host.unlink()
    result = run('--run-entry')
    assert result.returncode == 1 and 'cannot launch FEX companion' in result.stderr, result
    host.symlink_to('raven')
    result = run('--run-modules')
    assert result.returncode == 1 and 'FEX companion re-exec loop' in result.stderr, result
print('FEX launch modes, native isolation, missing companion and re-exec guard passed')
