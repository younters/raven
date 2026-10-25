#!/usr/bin/env python3
"""Run an isolated hardware fixture through DXMT (default) or DXVK and validate its output."""
from pathlib import Path
import json
import os
import resource
import shutil
import struct
import subprocess
import sys
import time


def main():
    raven, workload, root, work = (Path(value).resolve() for value in sys.argv[1:5])
    kind = sys.argv[5]
    backend = sys.argv[6] if len(sys.argv) > 6 else "DXMT"
    if backend not in ("DXMT", "DXVK"):
        raise ValueError("backend must be DXMT or DXVK")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    prefix = work / "prefix"
    env = os.environ | {"RAVEN_DXMT_ROOT": str(root) if backend == "DXMT" else "", "RAVEN_PREFIX": str(prefix)}
    env.pop("RAVEN_D3D_DIRECTORY", None)
    if backend == "DXVK":
        run = work / "run"
        run.mkdir()
        dlls = ("d3d9.dll",) if kind == "d3d9" else ("d3d11.dll", "dxgi.dll", "d3d10core.dll")
        for name in dlls:
            shutil.copy2(root / name, run / name)
        shutil.copy2(workload, run / workload.name)
        workload = run / workload.name
        env["DXVK_LOG_PATH"] = str(work)
    command = [str(raven), "--run-process", "--execution=native", "--timeout-ms=90000"]
    if kind in ("fullscreen", "present_rate", "pacing", "device10", "d3d9"):
        command.append("--desktop=macos")
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    result = subprocess.run(command + [str(workload)],
                            capture_output=True, text=True, timeout=110, env=env)
    elapsed = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    (work / "metrics.json").write_text(json.dumps({
        "backend": backend, "fixture": kind, "exit": result.returncode,
        "wall_seconds": elapsed,
        "host_guest_cpu_seconds": after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
        "icd": env.get("VK_DRIVER_FILES"),
    }, indent=2) + "\n")
    (work / "stdout.log").write_text(result.stdout)
    (work / "stderr.log").write_text(result.stderr)
    drive = prefix / "drive_c"
    diagnostics = result.stderr
    if backend == "DXVK":
        # The PE32 CRT writes driver logs in the Windows current directory rather than Raven's stderr.
        diagnostics += "\n".join(path.read_text(errors="replace") for path in sorted(drive.glob("*.log")))
        (work / "driver.log").write_text(diagnostics)
    if backend == "DXVK" and "DXVK:" not in diagnostics:
        raise RuntimeError("fixture did not identify the selected DXVK backend")
    output = {"device": "dxmt-device", "deferred": "triangle", "d3d9": "window"}.get(kind, kind)
    status = drive / f"{output}.txt"
    text = status.read_text() if status.exists() else "missing"
    if result.returncode or (kind not in ("present_rate", "pacing") and not text.startswith("OK")):
        raise SystemExit(f"{backend} {kind}: exit {result.returncode}, {text!r}\n{diagnostics[-3000:]}")
    scripts = Path(__file__).resolve().parents[4] / "scripts"
    if kind in ("triangle", "deferred", "geometry", "tessellation", "textured", "msaa"):
        checker = kind if kind in ("textured", "msaa") else "triangle"
        args = [sys.executable, str(scripts / f"check-d3d11-{checker}.py"), str(drive / f"{output}.raw")]
        if kind in ("geometry", "tessellation"):
            args += ["256", "256", "26", "26", "76", "0.5"]
        subprocess.run(args, check=True)
    elif kind == "d3d9":
        first, last = (drive / "window_first.raw").read_bytes(), (drive / "window_last.raw").read_bytes()
        if len(first) != 256 * 256 * 4 or len(last) != len(first):
            raise RuntimeError("incomplete D3D9 frame readback")
        center = last[(200 * 256 + 128) * 4:][:3]
        if last[:3] != bytes((0, 59 * 4, 0x40)) or center != bytes((255, 0, 0)) or first[:4] == last[:4]:
            raise RuntimeError("D3D9 readback did not contain the changing clears and fixed-function triangle")
        print(f"{backend} D3D9: all 60 presents completed; first/last GPU readback matched (screen pixels not checked)")
    elif kind == "compute":
        values = struct.unpack("<256I", (drive / "compute.raw").read_bytes())
        expected = [i * i + 7 + (255 if ((i // 8) % 8 + i % 8) % 2 == 0 else 240) for i in range(256)]
        if list(values) != expected:
            raise RuntimeError("compute dispatch did not write the 256 expected values")
        print(f"{backend} compute dispatch: all 256 values match")
    elif kind == "features":
        first, second = (drive / "features0.raw").read_bytes(), (drive / "features1.raw").read_bytes()
        samples = struct.unpack("<Q", (drive / "features_samples.raw").read_bytes())[0]
        if len(first) != 256 * 256 * 4 or len(second) != len(first):
            raise RuntimeError("incomplete multiple render targets")
        checks = [(first, 64, 128, (128, 0, 0, 128)), (first, 192, 128, (0, 128, 0, 128)),
                  (first, 128, 128, (0, 0, 0, 255)), (first, 64, 10, (0, 0, 0, 255)),
                  (second, 64, 128, (128, 128, 128, 255)), (second, 192, 128, (128, 128, 128, 255)),
                  (second, 128, 128, (0, 0, 255, 255))]
        for data, x, y, expected in checks:
            pixel = data[(y * 256 + x) * 4:][:4]
            if any(abs(a - b) > 3 for a, b in zip(pixel, expected)):
                raise RuntimeError(f"MRT/blend/mipmap pixel {x},{y}: {tuple(pixel)}, expected {expected}")
        if not 41000 <= samples <= 85000:
            raise RuntimeError(f"unexpected occlusion query sample count: {samples}")
        print(f"{backend} MRT, blending, mipmaps, indirect instancing and occlusion query ({samples} samples) passed")
    elif kind == "fullscreen":
        back = (drive / "fullscreen_back.txt").read_text()
        width, height = map(int, (drive / "fullscreen_full.txt").read_text().split("x"))
        if back != "256x256" or width < 640 or height < 480:
            raise RuntimeError(f"fullscreen/window sizes: {width}x{height}, {back}")
        for name, w, h, clear in [("full", width, height, (26, 102, 77)), ("back", 256, 256, (26, 51, 77))]:
            data = (drive / f"fullscreen_{name}.raw").read_bytes()
            if len(data) != w * h * 4 or any(abs(a - b) > 2 for a, b in zip(data[:3], clear)):
                raise RuntimeError(f"incorrect fullscreen {name} frame")
            center = data[((h // 2) * w + w // 2) * 4:][:3]
            if all(abs(a - b) <= 2 for a, b in zip(center, clear)):
                raise RuntimeError(f"fullscreen {name}: triangle is missing")
        print(f"{backend} fullscreen {width}x{height} and return to window passed")
    elif kind == "present_rate":
        frames, microseconds = map(int, text.split())
        if frames != 2000 or microseconds <= 0:
            raise RuntimeError(f"invalid presentation measurement: {text}")
        print(f"{backend}: {frames} timed presents, {frames * 1000000 / microseconds:.1f} presents/s")
    elif kind == "device10":
        for name in ("device10", "device10_1"):
            data = (drive / f"{name}.raw").read_bytes()
            if len(data) != 64 or any(abs(value - (64, 128, 191, 255)[i % 4]) > 1 for i, value in enumerate(data)):
                raise RuntimeError(f"{name} did not clear and read back the expected pixels")
        print(text)
    elif kind == "pacing":
        frames, microseconds = map(int, text.split())
        if frames != 16 or not 200000 <= microseconds < 10000000:
            raise RuntimeError(f"minimum-duration presents were not paced: {text}")
        print(f"{backend} minimum-duration pacing: {frames} presents in {microseconds / 1000:.1f} ms")
    else:
        print(text)


if __name__ == "__main__":
    main()
