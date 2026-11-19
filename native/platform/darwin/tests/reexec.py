import ctypes
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    system = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
    responsible = system.responsibility_get_pid_responsible_for_pid
    system.responsibility_spawnattrs_setdisclaim
except (OSError, AttributeError):
    print('SKIP: responsibility SPI unavailable', file=sys.stderr)
    sys.exit(77)
responsible.argtypes = [ctypes.c_int]
responsible.restype = ctypes.c_int

with tempfile.TemporaryFile() as inherited, tempfile.TemporaryFile() as channel, tempfile.TemporaryFile() as status, tempfile.TemporaryDirectory() as work:
    inherited.write(b'input')
    inherited.seek(0)
    descriptors = (inherited.fileno(), channel.fileno(), status.fileno())
    env = os.environ.copy()
    env['RAVEN_TEST_TICKET_REPORT'] = str(Path(work) / 'ticket')
    report = Path(work) / 'responsibility'
    env['RAVEN_TEST_RESPONSIBILITY_REPORT'] = str(report)
    for key in ('RAVEN_RESPONSIBILITY_CHECKED', 'RAVEN_FOREGROUND_CHILD', 'RAVEN_APP_NAME', 'RAVEN_APP_ICON', 'RAVEN_FOREGROUND_TICKET', 'RAVEN_FOREGROUND_LEASE_FD', 'RAVEN_FOREGROUND_LEASE_PATH', 'RAVEN_TEST_ORIGINAL_PID'):
        env.pop(key, None)
    process = subprocess.Popen(['deliberately-wrong-argv-zero', '--run-process', '--execution=native', '--desktop=macos', '--stdio=inherit',
                                f'--inherit-fd={descriptors[0]}', f'--handle-channel-fd={descriptors[1]}',
                                f'--exit-code-fd={descriptors[2]}', 'fixture.exe'], executable=sys.argv[1],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               pass_fds=descriptors, env=env)
    try:
        deadline = time.monotonic() + 20
        while (not report.exists() or len(report.read_text().split()) != 2) and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        if process.poll() == 77:
            print(process.communicate()[1].decode(), file=sys.stderr)
            sys.exit(77)
        if not report.exists():
            stdout, stderr = process.communicate(b'stdin', timeout=2)
            raise AssertionError(f'reexec failed before responsibility check: {process.returncode}: {stderr.decode()}')
        pid, owner = map(int, report.read_text().split())
        assert pid == owner == process.pid and responsible(process.pid) == process.pid, (pid, owner, process.pid)
        stdout, stderr = process.communicate(b'stdin', timeout=20)
        result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    channel.seek(0)
    status.seek(0)
    assert (result.returncode, result.stdout, result.stderr, channel.read(), status.read()) == (
        37, b'stdout', b'stderr', b'channel', b'exit'), result
    ticket = Path(env['RAVEN_TEST_TICKET_REPORT']).read_text()
    assert ticket and not Path(ticket).exists() and not Path(ticket + '.lease').exists(), 'unused foreground ticket leaked'
