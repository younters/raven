#!/usr/bin/env python3
"""Verify DXMT's IOSurface handoff and the actual Cocoa window, before and after a swap-chain resize."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time
from PIL import Image


def wait_file(path, process, deadline):
    while time.monotonic() < deadline:
        if path.exists() and path.read_text() == "OK":
            return
        if process.poll() is not None:
            raise RuntimeError(f"fixture exited {process.returncode} before {path.name}")
        time.sleep(.05)
    raise RuntimeError(f"fixture did not publish {path.name}")


def compare(actual, expected, label):
    if len(actual) != len(expected):
        raise RuntimeError(f"{label}: {len(actual)} bytes, expected {len(expected)}")
    # Presentation's fullscreen blit can differ by one quantization unit from the back buffer.
    wrong = sum(abs(a - b) > 3 for a, b in zip(actual, expected))
    if wrong > len(actual) // 500:
        raise RuntimeError(f"{label}: {wrong} channels differ by more than three")


def capture(work, info_tool, process, width, height, name):
    expected = (work / "prefix/drive_c" / f"window_{name}.raw").read_bytes()
    surface = work / "surfaces" / f"{width}x{height}.rgba"
    deadline = time.monotonic() + 5
    while True:
        try:
            compare(surface.read_bytes(), expected, f"{name} imported IOSurface")
            break
        except (FileNotFoundError, RuntimeError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.05)
    info = json.loads(subprocess.check_output([str(info_tool), str(process.pid)], text=True))
    shot = work / f"{name}-window.png"
    subprocess.run(["/usr/sbin/screencapture", "-x", "-o", "-l", str(info["id"]), str(shot)], check=True, capture_output=True)
    image = Image.open(shot).convert("RGB")
    logical = (round(info["bounds"]["Width"]), round(info["bounds"]["Height"]))
    image = image.resize(logical, Image.Resampling.BOX)
    reference = Image.frombytes("RGBA", (width, height), expected).convert("RGB")
    # Locate the Windows client inside Raven's drawn frame. Exclude edges where rasterization/scaling can mix pixels.
    points = [(x, y) for y in range(12, height - 12, 17) for x in range(12, width - 12, 17)]
    best = None
    for top in range(max(0, image.height - height) + 1):
        for left in range(max(0, image.width - width) + 1):
            good = sum(max(abs(a - b) for a, b in zip(image.getpixel((left + x, top + y)), reference.getpixel((x, y)))) <= 12
                       for x, y in points)
            if best is None or good > best[0]:
                best = (good, left, top)
    if best is None or best[0] < len(points) * .97:
        raise RuntimeError(f"{name}: screen pixels do not match the rendered triangle ({best}, {len(points)} samples)")
    _, left, top = best
    image.crop((left, top, left + width, top + height)).save(work / f"{name}-client.png")
    print(f"{name}: {width}x{height} IOSurface and Cocoa screen capture match the GPU readback", flush=True)


def main():
    raven, workload, root, work, info_tool = (Path(value).resolve() for value in sys.argv[1:6])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    env = os.environ | {"RAVEN_DXMT_ROOT": str(root), "RAVEN_PREFIX": str(work / "prefix"), "RAVEN_TRACE_DXMT": "1",
                        "RAVEN_DXMT_CAPTURE_DIRECTORY": str(work / "surfaces")}
    env.pop("RAVEN_D3D_DIRECTORY", None)
    with (work / "run.log").open("w") as log:
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=90000", "--desktop=macos",
                                    str(workload)], stdout=log, stderr=log, env=env)
        try:
            wait_file(work / "prefix/drive_c/window.txt", process, time.monotonic() + 30)
            capture(work, info_tool, process, 256, 256, "last")
            wait_file(work / "prefix/drive_c/window_resized.txt", process, time.monotonic() + 20)
            capture(work, info_tool, process, 320, 200, "resized")
            if process.wait(timeout=35):
                raise RuntimeError(f"window fixture exited {process.returncode}")
        except BaseException:
            process.kill()
            process.wait()
            raise
    if "0 presentation errors" not in (work / "run.log").read_text():
        raise RuntimeError("presentation did not finish without errors")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        raise SystemExit(f"DXMT presentation: {error}")
