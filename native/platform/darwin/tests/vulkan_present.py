#!/usr/bin/env python3
"""Verify that Windows Vulkan frames reach the Cocoa Metal child view, including after resize.

Requires an unlocked macOS desktop, screen capture permission, and Pillow. Only the matched client areas are retained.
"""
from pathlib import Path
from io import BytesIO
import os
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageCms, ImageDraw


def compare(screen, width, height, phase):
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    # screencapture reports physical pixels, including Retina scaling.
    for scale in (1, 2):
        w, h = width * scale, height * scale
        expected = Image.new("RGB", (w, h))
        draw = ImageDraw.Draw(expected)
        for quadrant in range(4):
            x, y = quadrant % 2 * w // 2, quadrant // 2 * h // 2
            draw.rectangle((x, y, x + w // 2 - 1, y + h // 2 - 1), fill=colors[(quadrant + phase) % 4])
        difference = ImageChops.difference(screen, Image.new("RGB", screen.size, colors[phase]))
        channels = [channel.point(lambda p: 255 if p <= 4 else 0) for channel in difference.split()]
        mask = ImageChops.darker(ImageChops.darker(channels[0], channels[1]), channels[2]).tobytes()
        for y in range(screen.height - h + 1):
            row = mask[y * screen.width:(y + 1) * screen.width]
            run = b"\xff" * (w // 2 - 2 * scale)
            x = row.find(run)
            while x >= 0 and x + w <= screen.width:
                # Only a run's top left can be the client area's top left.
                if not (x and row[x - 1]) and not (y and mask[(y - 1) * screen.width + x]):
                    # Cocoa rounds the lower window corners and antialiases edges. Check each quadrant's interior.
                    margin = 16 * scale
                    shown = screen.crop((x, y, x + w, y + h))
                    matches = True
                    for quadrant in range(4):
                        left, top = quadrant % 2 * w // 2, quadrant // 2 * h // 2
                        box = (left + margin, top + margin, left + w // 2 - margin, top + h // 2 - margin)
                        if any(high > 4 for low, high in ImageChops.difference(shown.crop(box), expected.crop(box)).getextrema()):
                            matches = False
                            break
                    if matches:
                        return shown
                x = row.find(run, x + 1)
    return None


def main():
    runtime, fixture, work = map(lambda value: Path(value).resolve(), sys.argv[1:])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    prefix = work / "prefix"
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    with (work / "runtime.log").open("w+") as log:
        process = subprocess.Popen([str(runtime), "--run-process", "--execution=native", "--timeout-ms=45000",
                                    "--desktop=macos", str(fixture)], env=env, stdout=log, stderr=log)
        try:
            for phase, (label, width, height, signal) in enumerate([
                ("first", 256, 192, "next"), ("resized", 320, 200, "done")
            ]):
                deadline = time.monotonic() + 15
                status = prefix / "drive_c" / "vulkan-present.txt"
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f"Vulkan client ended before {label}")
                    if status.exists() and status.read_text() == label:
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(f"Vulkan client did not present {label}")
                matched = None
                capture = work / "screen.png"
                try:
                    for attempt in range(5):
                        time.sleep(0.15)
                        subprocess.run(["/usr/sbin/screencapture", "-m", "-x", str(capture)], check=True, timeout=5)
                        with Image.open(capture) as image:
                            # Display-profile RGB is different from the Vulkan surface's sRGB values.
                            if "icc_profile" in image.info:
                                image = ImageCms.profileToProfile(image, ImageCms.ImageCmsProfile(BytesIO(image.info["icc_profile"])),
                                                                 ImageCms.createProfile("sRGB"), outputMode="RGB")
                            matched = compare(image.convert("RGB"), width, height, phase)
                        if matched is not None:
                            break
                finally:
                    capture.unlink(missing_ok=True)
                if matched is None:
                    raise RuntimeError(f"{label}: displayed Vulkan pixels did not match all four quadrants")
                matched.save(work / f"{label}.png")
                print(f"{label}: four quadrant interiors match the {width} x {height} presented Vulkan frame")
                (prefix / "drive_c" / f"vulkan-present-{signal}").touch()
            process.wait(timeout=10)
            log.seek(0)
            output = log.read()
            if process.returncode or "Native entry returned 0 " not in output or "VUID-" in output or "Validation Error:" in output:
                raise RuntimeError(f"Vulkan client failed: {output}")
        except Exception:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
