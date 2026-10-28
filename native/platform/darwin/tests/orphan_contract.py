#!/usr/bin/env python3
"""Kill only the host started here and inspect its Windows child's response."""
import os
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time

raven, fixture, mode = sys.argv[1], sys.argv[2], int(sys.argv[3])
with tempfile.TemporaryDirectory(prefix='raven-orphan-contract-') as prefix:
    env = dict(os.environ, RAVEN_PREFIX=prefix)
    command = [raven, '--run-process', '--execution=native', '--timeout-ms=15000', '--stdio=inherit', fixture, '--', '-p', str(mode)]
    host = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
    output = queue.Queue()
    def read_output():
        for line in host.stdout:
            output.put(line)
    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    child = None
    try:
        deadline = time.monotonic()+12
        while time.monotonic()<deadline:
            try: line = output.get(timeout=0.2)
            except queue.Empty:
                if host.poll() is not None: break
                continue
            print(line, end='', flush=True)
            fields=line.split()
            if len(fields)==3 and fields[:2]==['READY',str(mode)]:
                child=int(fields[2]); break
        if child is None: raise AssertionError('guest child did not report READY')
        os.kill(host.pid,signal.SIGKILL)
        host.wait(timeout=5)
        if mode==3:
            time.sleep(0.5)
            os.kill(child,0)  # a child without a wait must survive its parent's exit
        deadline=time.monotonic()+7
        passed=False
        while time.monotonic()<deadline:
            try: line=output.get(timeout=0.2)
            except queue.Empty: continue
            print(line,end='',flush=True)
            if line.split()[:2]==['PASS',str(mode)]: passed=True; break
        if not passed: raise AssertionError('child did not complete the orphan contract')
        deadline=time.monotonic()+2
        while time.monotonic()<deadline:
            command_line=subprocess.run(['ps','-p',str(child),'-o','command='],capture_output=True,text=True).stdout
            if '--run-process' not in command_line or fixture not in command_line:
                break
            time.sleep(0.1)
        else:
            raise AssertionError('child host stayed alive after reporting PASS')
    finally:
        if host.poll() is None:
            host.kill(); host.wait()
        if child is not None:
            command_line = subprocess.run(['ps', '-p', str(child), '-o', 'command='], capture_output=True, text=True).stdout
            if '--run-process' in command_line and fixture in command_line:
                try: os.kill(child, signal.SIGKILL)
                except ProcessLookupError: pass
