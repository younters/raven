#!/usr/bin/env python3
"""Bounded cooperative integration checks; only the caller's process group is killed."""
import os
import signal
import struct
import subprocess
import sys
import tempfile


def run(raven, fixture, sidecar, **knobs):
    env = {k: v for k, v in os.environ.items() if not k.startswith("X87_")}
    env.update(RAVEN_TRACE_API="1", **knobs)
    env.pop("RAVEN_X87_SIDECAR", None)
    if sidecar is not None:
        env["RAVEN_X87_SIDECAR"] = sidecar
    process = subprocess.Popen([raven, "--run-process", "--execution=native", "--stdio=inherit", fixture],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = process.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        # Report an exec hang once. Never retry a timed-out launch.
        state = subprocess.run(["ps", "-axo", "pid,ppid,state,command"], capture_output=True, text=True).stdout
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise RuntimeError(f"x87 launch timed out; STOP, inspect for exec state U:\n{state}")
    text = err.decode(errors="replace")
    if process.returncode or len(out) != 24:
        raise RuntimeError(f"guest failed: {process.returncode}, stdout={out!r}\n{text}")
    checksum, ticks, frequency = struct.unpack("<QQQ", out)
    assert checksum == 0x415e848000000000, hex(checksum)
    return checksum, ticks / frequency, text


def main():
    raven, sidecar, fixture32, fixture64, case = sys.argv[1:]
    fixture = fixture64 if case == "x64" else fixture32
    if case == "child":
        fixture = fixture32.replace(".exe", "-child.exe")
    baseline = run(raven, fixture, "")
    if case == "fallback":
        with tempfile.TemporaryDirectory(prefix="raven-x87-home-") as home:
            absent = run(raven, fixture, None, HOME=home)
        assert absent[0] == baseline[0]
        assert "RUNNING WITHOUT X87 ACCELERATION" not in absent[2], absent[2]
        for path, knobs in [(sidecar, {"X87_FORCE_UNSUPPORTED": "1"}),
                            (sidecar, {"X87_FORCE_ABANDON": "entry"}),
                            ("/nonexistent/raven-x87sidecar", {})]:
            result = run(raven, fixture, path, **knobs)
            assert result[0] == baseline[0]
            assert "RUNNING WITHOUT X87 ACCELERATION" in result[2], result[2]
        print("unsupported, rolled-back hook and missing sidecar all preserve PE32 execution")
        return
    hooked = run(raven, fixture, sidecar)
    assert baseline[0] == hooked[0]
    assert "RUNNING WITHOUT X87 ACCELERATION" not in hooked[2], hooked[2]
    marker = "patched ranges invalidated"
    if case == "x64":
        assert marker not in hooked[2], hooked[2]
        print("PE64 bypasses sidecar and preserves the checksum")
    else:
        assert "2 patched ranges invalidated" in hooked[2], hooked[2]
        if case == "child":
            assert hooked[2].count("2 patched ranges invalidated") == 2, hooked[2]
        print(f"bit-identical checksum {hooked[0]:016x}; stock={baseline[1]:.6f}s hooked={hooked[1]:.6f}s speedup={baseline[1]/hooked[1]:.2f}x")


if __name__ == "__main__":
    main()
