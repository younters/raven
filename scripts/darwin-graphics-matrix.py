#!/usr/bin/env python3
"""Runs AIO Graphics Test once per graphics API and bitness and keeps the results, so builds can be compared.

    darwin-graphics-matrix.py --raven build-macos/raven --prefix <prefix> --aio <folder with the AIO executables> \
        --output <results folder> [--apis dx11,vk] [--bits 64,32] [--seconds 15] [--limit 240] [--compare earlier.csv]

Each run is one program start with one API. `--sweep` is avoided on purpose: AIO presents its other APIs through a
D3D11 window with a copy from the GPU, which measures that copy as much as the API. D3D9 needs mtld3d's DLLs beside
the executable, and D3D12 needs the environment of a D3D12 backend, passed with --env. A row records what happened
even if nothing ran, because a refusal or a crash is the result this table exists to show.

Frame rates alone have lied here, a black scene scored well, so the last frame of each run is saved beside its row
for a person to look at.
"""
import argparse
import csv
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

APIS = ("dx12", "dx11", "dx10", "dx9", "dx8", "dx7", "ddraw2d", "vk", "gl")
FIELDS = ("when", "build", "bits", "api", "outcome", "avg_fps", "min_fps", "max_fps", "low1pct_fps", "seconds", "notes")
EXECUTABLES = {"64": "AIO-Graphics-Test-64bit.exe", "32": "AIO-Graphics-Test-32bit.exe"}


def read_bench(results):
    """The figures of the run from AIO's CSV, whose third line holds them."""
    path = results / "AIO-Graphics-Test_bench.csv"
    if not path.exists():
        return None
    lines = path.read_text(errors="replace").splitlines()
    for index, line in enumerate(lines):
        if "avg_fps" in line and index + 1 < len(lines):
            names = [name.strip() for name in line.split(",")]
            values = [value.strip() for value in lines[index + 1].split(",")]
            return dict(zip(names, values))
    return None


def capture(pid, target):
    """Saves the window of the process as an image. Fails quietly: the row matters more than the picture."""
    owner = (
        'import Quartz,sys;'
        'w=Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll,0);'
        f'print(next((x["kCGWindowNumber"] for x in w if x.get("kCGWindowOwnerPID")=={pid} and x.get("kCGWindowBounds",{{}}).get("Width",0)>100),""))'
    )
    found = subprocess.run(["python3", "-c", owner], capture_output=True, text=True)
    window = found.stdout.strip()
    if window:
        subprocess.run(["screencapture", "-x", "-o", f"-l{window}", str(target)], capture_output=True)


def run_one(args, bits, api, folder):
    executable = args.aio / EXECUTABLES[bits]
    results = args.aio / "AIO Results" / "Benchmark"
    if results.exists():
        for old in results.iterdir():
            if old.is_file():
                old.unlink()
    log = folder / f"{bits}-{api}.log"
    environment = dict(os.environ, RAVEN_PARALLEL="1", RAVEN_PREFIX=str(args.prefix), RAVEN_CONTINUE_UNIMPLEMENTED="1")
    for item in args.env:
        name, _, value = item.partition("=")
        environment[name] = value
    command = [str(args.raven), "--run-process", "--execution=" + args.execution, f"--timeout-ms={args.limit * 1000}", "--desktop=macos", str(executable),
               "--", "--cube", api, "--bench", str(args.seconds), "--autoclose", "5"]
    started = time.time()
    with log.open("w") as output:
        process = subprocess.Popen(command, cwd=args.aio, env=environment, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        pictured = False
        outcome = None
        while True:
            code = process.poll()
            if code is not None:
                outcome = code
                break
            elapsed = time.time() - started
            if not pictured and read_bench(results) is None and elapsed > args.limit * 0.75:
                capture(process.pid, folder / f"{bits}-{api}-late.png")
                pictured = True
            if elapsed > args.limit:
                capture(process.pid, folder / f"{bits}-{api}-stuck.png")
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                outcome = "timeout"
                break
            time.sleep(1)
    seconds = round(time.time() - started)
    figures = read_bench(results)
    row = {"bits": bits, "api": api, "seconds": seconds, "notes": ""}
    if figures and figures.get("avg_fps"):
        row["outcome"] = "ran" if outcome == 0 else f"ran, then ended with {outcome}"
        for name in ("avg_fps", "min_fps", "max_fps", "low1pct_fps"):
            row[name] = figures.get(name, "")
        if results.exists():
            for image in results.glob("*.png"):
                shutil.copy(image, folder / f"{bits}-{api}.png")
    else:
        row["outcome"] = "timed out" if outcome == "timeout" else f"no result, ended with {outcome}"
        text = log.read_text(errors="replace")
        lines = [line for line in text.splitlines() if re.search(r"unimplemented|missing|error|fail|fault", line, re.I)]
        row["notes"] = lines[-1][:160] if lines else ""
    summary = results / "AIO-Graphics-Test_bench.csv"
    if summary.exists():
        shutil.copy(summary, folder / f"{bits}-{api}.csv")
    return row


def compare(rows, earlier_path):
    earlier = {}
    with open(earlier_path, newline="") as source:
        for row in csv.DictReader(source):
            earlier[(row["bits"], row["api"])] = row
    print("\nchange from", earlier_path)
    for row in rows:
        before = earlier.get((row["bits"], row["api"]))
        if not before:
            continue
        try:
            print(f"  {row['bits']}-bit {row['api']:8} {float(before['avg_fps']):8.1f} -> {float(row['avg_fps']):8.1f} FPS")
        except (KeyError, ValueError):
            print(f"  {row['bits']}-bit {row['api']:8} {before.get('outcome')} -> {row.get('outcome')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--raven", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--aio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apis", default=",".join(APIS))
    parser.add_argument("--bits", default="64,32")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--limit", type=int, default=240, help="seconds before a run counts as stuck")
    parser.add_argument("--execution", default="fex")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--build", default="")
    parser.add_argument("--compare", type=Path)
    args = parser.parse_args()
    args.raven = args.raven.resolve()
    args.aio = args.aio.resolve()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    folder = args.output / stamp
    folder.mkdir(parents=True)
    rows = []
    with (folder / "matrix.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, FIELDS)
        writer.writeheader()
        for bits in args.bits.split(","):
            for api in args.apis.split(","):
                row = run_one(args, bits, api, folder)
                row["when"] = stamp
                row["build"] = args.build
                writer.writerow(row)
                target.flush()
                rows.append(row)
                print(f"{bits}-bit {api:8} {row['outcome']:28} avg {row.get('avg_fps', '-'):>8} low1% {row.get('low1pct_fps', '-'):>8} "
                      f"{row['seconds']:4}s {row['notes']}")
                sys.stdout.flush()
    if args.compare:
        compare(rows, args.compare)
    print("results in", folder)


if __name__ == "__main__":
    main()
