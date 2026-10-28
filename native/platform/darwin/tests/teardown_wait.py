#!/usr/bin/env python3
"""A deadline while the main coroutine waits must finish host teardown."""
import os
import signal
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='raven-teardown-') as prefix:
    process = subprocess.Popen(
        [sys.argv[1], '--run-process', '--execution=native', '--timeout-ms=1000',
         '--desktop=macos', sys.argv[2]],
        env=dict(os.environ, RAVEN_PREFIX=prefix), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = process.communicate(timeout=20)
        print(output, end='')
        if process.returncode == 0 or 'native entry timed out' not in output:
            raise AssertionError('expected the runtime deadline to terminate the process')
    finally:
        if process.poll() is None:
            # Only descendants of the host started here; no command-line matching.
            rows = [line.split(None, 2) for line in subprocess.check_output(
                ['ps', '-axo', 'pid=,ppid=,command='], text=True).splitlines()]
            owned = {process.pid}
            while True:
                found = owned | {int(pid) for pid, parent, _ in rows if int(parent) in owned}
                if found == owned:
                    break
                owned = found
            for pid in owned:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait()
