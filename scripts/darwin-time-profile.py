#!/usr/bin/env python3
"""Where a running Raven program's time goes, window by window, on macOS.

Takes a call-stack sample of the process every so often and sorts each thread's time into categories, so a slow
phase can be named from evidence. It needs no special build: it reads the stacks macOS `sample` reports.

    darwin-time-profile.py <pid> [--every 60] [--length 10] [--windows 0] [--output report.csv] [--map dir] [--nm llvm-nm]

Each row counts one thread's busy samples. Idle time is left out, so the shares say what the busy threads were doing.

Start the program with RAVEN_PROFILE_MAP=<dir> and pass --map <dir> to tell the translator from the code it generated:
Raven records there where it loaded the translator, whose functions macOS cannot name, and --nm lists them from the file.
"""
import argparse
import bisect
import collections
import csv
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

IDLE = ("__psynch_cvwait", "__workq_kernreturn", "semaphore_wait_trap", "semaphore_timedwait_trap", "mach_msg2_trap",
        "__semwait_signal", "kevent", "poll", "__select", "__ulock_wait", "os_sync_wait_on_address", "__sigsuspend")

# Short names of system calls, which must match whole: "read" is also part of "pthread".
SYSTEM_CALLS = {name: "file reads and writes" for name in ("read", "write", "pread", "pwrite", "open", "close", "fstat", "stat", "lstat", "readv", "writev", "fsync", "lseek", "ftruncate", "__ioctl", "ioctl")}

# Read from the innermost frame out: the first frame that matches decides, so a lock inside a graphics call counts as lock wait.
RULES = (
    ("lock wait", ("__psynch_mutexwait", "__psynch_rw_", "_pthread_mutex_firstfit_lock_wait", "__ulock_wait")),
    ("fault or signal", ("_sigtramp", "sigprocmask", "__sigaltstack", "sigreturn", "siglongjmp", "sigsetjmp", "longjmp")),
    ("memory mapping", ("__mprotect", "__mmap", "__munmap", "mach_vm_", "madvise", "notify_memory", "commit_allocate", "commit_map_view",
                        "FexProcess::protect_memory", "FexProcess::map_memory", "FexProcess::unmap_memory", "NativeMemory::")),
    ("file reads and writes", ("__pread", "pread", "__read", "__write", "read_nocancel", "__open", "__stat", "__getattrlist", "__fstat", "lseek", "__close")),
    ("shader compile", ("MTLCompiler", "newLibrary", "newRenderPipeline", "newComputePipeline", "MTLFunction", "airconv", "sm50_", "LLVM", "llvm::",
                        "SM50", "newDefaultLibrary", "SerializedLibrary")),
    ("gpu submission", ("IOGPU", "iokit_user_client_trap", "IOConnectCall", "MTLCommand", "_MTL", "AGX", "Metal")),
    ("window system", ("AppKit", "NSApplication", "CoreAnimation", "CA::", "HIToolbox", "SkyLight", "CGS", "cocoa")),
    ("api checks", ("UnixRanges::check", "raven_unix_checked", "raven_unix_mapped_range", "validate_write_memory")),
    ("guest memory copies", ("_platform_memmove", "_platform_memcpy", "_platform_memset", "__bzero", "write_memory", "read_memory")),
    ("scheduling", ("NativeThreads::", "pthread_cond", "__psynch_cv", "FexProcess::resume", "FexProcess::set_context", "swtch", "thread_switch")),
    ("raven api", ("ApiRegistry::dispatch", "run_native_process", "raven::", "dispatch(")),
    ("allocator", ("malloc", "free_tiny", "tiny_", "szone", "nanov2", "free_small")),
)


class Translator:
    """Where Raven put the translator in one process, and the names of its functions from the file."""

    def __init__(self, map_folder, pid, nm):
        self.base = self.size = 0
        self.preferred = 0
        self.addresses = []
        self.names = []
        path = Path(map_folder) / f"{pid}.map"
        if not path.exists():
            return
        base, size, module = path.read_text().splitlines()[-1].split(" ", 2)
        self.base, self.size = int(base, 16), int(size)
        # The file's addresses count from the base the PE header asks for.
        header = Path(module).read_bytes()[:4096]
        self.preferred = int.from_bytes(header[int.from_bytes(header[0x3c:0x40], "little") + 48:][:8], "little")
        listing = subprocess.run([nm, "--demangle", "-n", module], capture_output=True, text=True).stdout
        for line in listing.splitlines():
            fields = line.split(" ", 2)
            if len(fields) != 3:
                continue
            if fields[1] in "tT":
                self.addresses.append(int(fields[0], 16))
                self.names.append(fields[2])

    def name(self, address):
        """The function a frame is in, or None for a frame outside the translator."""
        if not self.base <= address < self.base + self.size:
            return None
        index = bisect.bisect_right(self.addresses, address - self.base + self.preferred) - 1
        return self.names[index] if index >= 0 else "?"


def where(address, translator):
    """The category of a frame macOS could not name: the translator's own code, or code it generated."""
    name = translator.name(address) if translator else None
    return ("translator", name) if name else ("guest code", "generated code")


def classify(stack, translator):
    """stack is innermost first: pairs of symbol text and address, the address set only for a frame without a symbol.

    The innermost frame that a rule recognizes decides. Unnamed frames are code Raven loaded or the translator made: a
    sample that reaches one before any rule was running there, or in a host function it called and no rule knows."""
    for symbol, address in stack:
        if symbol == "???":
            return where(address, translator)
        if symbol in SYSTEM_CALLS:
            return SYSTEM_CALLS[symbol], symbol
        for category, needles in RULES:
            if any(needle in symbol for needle in needles):
                return category, symbol
    return "other", stack[0][0]


def parse(path, translator):
    """The busy samples of each category and the busiest symbols, from the tree in a report of `sample`."""
    lines = Path(path).read_text(errors="replace").split("\n")
    start = next((i for i, line in enumerate(lines) if line.startswith("Call graph:")), None)
    if start is None:
        return None
    end = next((i for i, line in enumerate(lines) if line.startswith("Total number in stack")), len(lines))
    rows = []
    for line in lines[start + 1:end]:
        match = re.match(r"^([ +!:|]*)(\d+) (.*)$", line)
        if match:
            rows.append((len(match.group(1)), int(match.group(2)), match.group(3).strip()))
    totals = collections.Counter()
    threads = collections.Counter()
    leaves = collections.Counter()
    # A frame's own samples are its count less those of its children, so a frame with children can still hold time.
    open_frames = []
    thread = ""

    def close(frame):
        own = frame["count"] - frame["children"]
        if own <= 0 or not frame["stack"]:
            return
        symbol = frame["stack"][0][0]
        if any(symbol.startswith(word) or word in symbol for word in IDLE):
            return
        category, named = classify(frame["stack"], translator)
        totals[category] += own
        threads[frame["thread"]] += own
        leaves[f"{category}: {(named if symbol == '???' else symbol)[:80]}"] += own

    for depth, count, text in rows:
        while open_frames and open_frames[-1]["depth"] >= depth:
            close(open_frames.pop())
        if text.startswith("Thread_"):
            thread = text.split()[0]
            open_frames.append({"depth": depth, "count": count, "children": 0, "stack": [], "thread": thread})
            continue
        name = re.sub(r"\s+\(in .*", "", text)
        address = re.search(r"\[0x([0-9a-f]+)", text)
        name = re.sub(r"\s+\[0x.*", "", name).strip()
        symbol = "???" if name.startswith("???") else name
        frame = (symbol, int(address.group(1), 16) if address and symbol == "???" else 0)
        parent = open_frames[-1] if open_frames else None
        if parent:
            parent["children"] += count
        open_frames.append({"depth": depth, "count": count, "children": 0, "stack": [frame] + (parent["stack"] if parent else []), "thread": thread})
    while open_frames:
        close(open_frames.pop())
    return totals, threads, leaves


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pid", type=int)
    parser.add_argument("--every", type=float, default=60, help="seconds between the starts of samples")
    parser.add_argument("--length", type=int, default=10, help="seconds each sample lasts")
    parser.add_argument("--windows", type=int, default=0, help="samples to take; 0 runs until the process ends")
    parser.add_argument("--output", type=Path, help="write one CSV row per window and category")
    parser.add_argument("--map", help="the folder given to the program as RAVEN_PROFILE_MAP")
    parser.add_argument("--nm", default="llvm-nm", help="llvm-nm, to read the translator's function names")
    parser.add_argument("--top", type=int, default=6, help="busiest symbols to list per window")
    args = parser.parse_args()

    began = time.time()
    table = collections.defaultdict(collections.Counter)
    grand = collections.Counter()
    window = 0
    writer = None
    output = None
    if args.output:
        output = args.output.open("w", newline="")
        writer = csv.writer(output)
        writer.writerow(["seconds", "category", "samples", "share"])
    translator = Translator(args.map, args.pid, args.nm) if args.map else None
    while args.windows == 0 or window < args.windows:
        started = time.time()
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "sample.txt"
            result = subprocess.run(["sample", str(args.pid), str(args.length), "-file", str(report)], capture_output=True)
            if result.returncode or not report.exists():
                break
            parsed = parse(report, translator)
        if not parsed:
            break
        totals, threads, leaves = parsed
        at = int(started - began)
        busy = sum(totals.values()) or 1
        print(f"--- t={at}s busy samples {busy} on {len(threads)} threads")
        for category, count in totals.most_common():
            print(f"  {100 * count / busy:5.1f}%  {category}")
            if writer:
                writer.writerow([at, category, count, f"{count / busy:.4f}"])
        for name, count in leaves.most_common(args.top):
            print(f"       {100 * count / busy:5.1f}%  {name}")
        sys.stdout.flush()
        if output:
            output.flush()
        table[at] = totals
        grand.update(totals)
        window += 1
        remaining = args.every - (time.time() - started)
        if remaining > 0 and (args.windows == 0 or window < args.windows):
            time.sleep(remaining)
    busy = sum(grand.values()) or 1
    print(f"=== all {window} windows, {busy} busy samples")
    for category, count in grand.most_common():
        print(f"  {100 * count / busy:5.1f}%  {category}")


if __name__ == "__main__":
    main()
