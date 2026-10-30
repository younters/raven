#!/usr/bin/env python3
"""A deadline while the main coroutine waits must finish host teardown."""
import os
import signal
import subprocess
import sys
import tempfile

def owned_pids(snapshot, root):
    parents = []
    for line in snapshot.splitlines():
        fields = line.split(None, 2)
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            parents.append((int(fields[0]), int(fields[1])))
    owned = {root}
    while True:
        found = owned | {pid for pid, parent in parents if parent in owned}
        if found == owned:
            return owned
        owned = found


if len(sys.argv) == 2 and sys.argv[1] == '--check-ps':
    assert owned_pids('10 1 root\n11 10\n12 11 child\n99\n\nheader row\n13 9 other', 10) == {10, 11, 12}
    sys.exit(0)

with tempfile.TemporaryDirectory(prefix='raven-teardown-') as prefix:
    process = subprocess.Popen(
        [sys.argv[1], '--run-process', '--execution=native', '--timeout-ms=1000',
         '--desktop=macos', '--command-line=teardown ' + ('multi' if len(sys.argv)>3 else 'single'), sys.argv[2]],
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
            rows = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='], text=True)
            owned = owned_pids(rows, process.pid)
            for pid in owned:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait()
