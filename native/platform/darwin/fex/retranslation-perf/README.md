# Translation retention investigation

Base: perf/fex-chrome-call-costs, 8d6d8690. Private modules live at
`/tmp/raven-retranslation-modules`; shared raven-components was not modified.
Patches 0060–0062 are independent of the other agent's reserved 0040–0049.

`RAVEN_FEX_PROFILE_COMPILE=1` collects compilation attempts by guest RIP and
reports 4 KiB page buckets: unique RIPs, additional attempts at those RIPs, and
the number of RIPs compiled more than once. Unique counts are per process,
including its threads; sums across processes do not deduplicate DLL addresses.
Repeated attempts include compile races and code-buffer rollover, not just SMC.
Profile records are pulled through RavenCompileProfile at exit, independently
of FEX's disabled-by-default logger. Module ranges carry process IDs; the parser
never matches one process's anonymous code against another process's images.

```
python3 scripts/analyze-fex-retranslation.py /tmp/raven-retranslation-after.fex-0.log
```

Invalidation reasons: 1 protection, 2 DEP, 3 section-unmap, 4 free,
5 explicit flush/dirty/inline SMC, 6 write fault, 7 untracked write,
8 code-cache clear (size distinguishes a new buffer from clearing the map).
Reason 5 is an aggregate of the common aligned-interval API; it does not identify
which of its callers triggered each event. There is no per-block causal history.
Disabled profiling performs no map insertions or clock reads.

The pinned FEX tracker already aligns to 4096 bytes, independently of Darwin's
16384-byte physical pages. Guest-to-host and thread lookup invalidation operate
on those guest-page ranges; their normal range invalidation does not clear all
translations. Full code-buffer clears are separately counted.

Patch 0061 skips notifications covering an already executable, entirely
non-writable interval when the new protection is also non-writable executable.
It removes stale RWX interval membership when a page becomes RX. It retains
invalidation when bytes could have changed, when execution rights change, and
on explicit flushes, dirty notifications, faults, and frees. In particular, it
does not skip RW -> RX, RWX -> RX, or RWX -> RWX. A non-null zero-length aligned
invalidation is a no-op; null still means the whole process.

Raven's guest FlushInstructionCache and NtFlushInstructionCache now forward
local requests to the translator callbacks instead of succeeding without any
notification. Ordinary native execution keeps its existing behavior.

The regression starts with an RWX allocation, then protects the code RX to
exercise removal of stale writable-code tracking. It runs a guest program in
serial and parallel mode: code and data
occupy distinct 4 KiB pages of one 16 KiB host page. Data protection changes,
data writes and data flushes must retain the code; RX-only protection changes
must also retain it. A real rewrite must return the new immediate and compile
exactly one additional block. Existing x32/x64 JIT stress tests also exercise
writable code modification with flushes.

Upstream inspected on 2026-10-31:
- https://github.com/FEX-Emu/FEX/blob/main/Source/Windows/Common/InvalidationTracker.cpp
- https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Core/LookupCache.h
- https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/signal_arm64ec.c

Current upstream protection invalidation has the same unconditional executable
notification invalidation as the pin; no newer fix for this case was found.
Wine ARM64EC brackets protection changes with notifications and forwards local
successful instruction-cache flushes to BTCpu64FlushInstructionCache. Raven's
fix follows this protocol rather than changing 16 KiB host protection trapping.

The checked-in RESULTS.txt records a missing before split and incomplete after
detail output. The parser compares page-attempt sums with timer totals and
reports the coverage gap rather than treating partial records as complete.
Large exit dumps should be replaced by bounded summaries before another causal
Chrome measurement; the three-sample budget was not extended.

## Shared-view and flush review follow-up

Patch 0063 adds SharedIntervals separately from RWXIntervals. map_shared registers
its host interval through RavenMarkSharedMemory in both x64 and WoW64 modules.
An executable shared view reports byte validation regardless of whether that
virtual view is RX. FEX's existing Darwin inline SMC guards check the bytes at
execution and discard stale translations after writes through another alias or
process. The property is removed on unmapping. It does not grant guest write
permission or make the RX view eligible for RWX write-fault recovery. Ordinary
private/image RX mappings retain the 0061 optimization.

`native_alias_flush_x{32,64}_fex_{0,1}_{0,1}` covers local RW/RX aliases and a
child writing the shared section, both with and without explicit flushes and in
serial/parallel execution. It also covers real self/remote handles, invalid
handles, and unmapped, overflowing and partially mapped flush ranges.

`native_flush-only_fex` and its parallel variant deliberately mutate the loader's
backing alias without a dirty/protection notification: execution must keep the
old value before the flush and return the new value afterward. The guest API
regression also requires an extra compilation after flushing unchanged private
RX code through a real self handle. This checks the API forwarding itself;
shared aliases alone would pass a return-value test using automatic validation.

A remote ProcessObject handle preserves successful Win32/NT flush status. Raven's
remote write_memory request already notifies the target; this does not add a
standalone remote-flush RPC. Local FEX flushes intersect logical mapped pages
before converting to host pointers, ignoring unmapped holes and overflow tails.

The final review benchmark pair is FAILED and cannot be used for performance
comparison. `review-benchmark-failed.json` retains the exit codes and missing FEX
result; macOS power logs show the machine repeatedly sleeping during both lanes.
See RESULTS.txt for complete qualification and remaining screenshot failures.
