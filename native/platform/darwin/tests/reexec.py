import os
import subprocess
import sys
import tempfile
from pathlib import Path

with tempfile.TemporaryFile() as inherited, tempfile.TemporaryFile() as channel, tempfile.TemporaryFile() as status, tempfile.TemporaryDirectory() as work:
    inherited.write(b'input')
    inherited.seek(0)
    descriptors = (inherited.fileno(), channel.fileno(), status.fileno())
    env = os.environ.copy()
    env['RAVEN_TEST_TICKET_REPORT'] = str(Path(work) / 'ticket')
    for key in ('RAVEN_RESPONSIBILITY_CHECKED', 'RAVEN_FOREGROUND_CHILD', 'RAVEN_APP_NAME', 'RAVEN_APP_ICON'):
        env.pop(key, None)
    result = subprocess.run([sys.argv[1], '--run-process', '--execution=native', '--desktop=macos', '--stdio=inherit',
                             f'--inherit-fd={descriptors[0]}', f'--handle-channel-fd={descriptors[1]}',
                             f'--exit-code-fd={descriptors[2]}', 'fixture.exe'],
                            input=b'stdin', capture_output=True, pass_fds=descriptors, env=env, timeout=20)
    channel.seek(0)
    status.seek(0)
    assert (result.returncode, result.stdout, result.stderr, channel.read(), status.read()) == (
        37, b'stdout', b'stderr', b'channel', b'exit'), result
    ticket = Path(env['RAVEN_TEST_TICKET_REPORT']).read_text()
    assert ticket and not Path(ticket).exists() and not Path(ticket + '.lease').exists(), 'unused foreground ticket leaked'
