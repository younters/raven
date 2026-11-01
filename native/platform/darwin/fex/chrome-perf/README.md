# Chrome performance investigation

Worktree: `/Users/yount/.t3/worktrees/raven/t3-a7d840dc`
Branch: `perf/chrome-fex-investigation`, based on macos 8b1cfca2.

Stable offline harness (standard library only):

```
cp -cR /Users/yount/Documents/Projects/raven-fixtures/chrome /tmp/YOUR-OWN-chrome-prefix
RAVEN_FEX_PERF=1 python3 scripts/benchmark-chrome-fex.py \
  --raven build-perf/native/raven --prefix /tmp/YOUR-OWN-chrome-prefix \
  --modules /Users/yount/Documents/Projects/raven-components/fex-modules \
  --output /tmp/YOUR-OWN-result.json --samples 1 --timeout 120
```

Do not share the writable prefix or output with another run. The HTML at
`scripts/benchmarks/chrome-js.html` is Raven's existing synthetic prime/sort/string/
JSON workload, made synchronous with an explicit checked result. It is **not** a
JetStream or Speedometer score. Uses headless Chrome, GPU enabled, parallel mode,
and a fresh Chrome profile per launch; checks prime count, string length, item
count. `--stdio=inherit` is necessary for Chrome's `--dump-dom` output. Both engines
use the same executable; JSON records executable/module/page hashes. One sample
is exploratory only, not statistical evidence. Do not run builds or other
benchmarks concurrently for comparable timings. Counters are inclusive and may
overlap; they report at normal process exit, not on forced termination.

Initial valid instrumented baseline: `/tmp/raven-chrome-profile.json` and adjacent
logs. Rosetta JS 1223.2 ms / launch 7.321 s; FEX JS 6850.7 ms / launch 49.008 s.
After contiguous image-protection runs (64aa5b8b):
`/tmp/raven-chrome-runs.json`. Rosetta JS 1170.1 ms / launch 7.181 s;
FEX JS 6749 ms / launch 47.927 s. JS difference is within single-run noise.
Across exiting FEX processes, protection notifications fell from 1,127,162 to
13,840; inclusive protect time fell from 2.018 s to 0.800 s. Child counts and
scheduling differ, so these totals are diagnostic rather than controlled latency.

The earlier `/tmp/raven-chrome-baseline.json` lacks DOM results and is INVALID.
M5 smoke outputs: `/tmp/raven-chrome-m5.json`,
`/tmp/raven-chrome-startup/result.json`; these overlapped other activity and are
exploratory, not a reliable performance baseline. `--smoke --samples 1` skips
warmups and allows bounded runs in both existing M5 scripts.

Targeted checks after image coalescing: 14/14 passed (7 tests + cleanup fixtures),
`/tmp/raven-chrome-targeted.log`. Final area/full suites are deferred until the
multiblock repair. No FEX module changes yet; 0028 remains active pending repair.
Agent scope: multiblock and SMC/W^X; reserved patch numbers 0040–0049.

## Multiblock continuation: negative controls (2026-10-31)

The supplied uncommitted 0040 is only the inverse of 0028: it enables
multiblock without a snapshot repair. Do not qualify it as a correctness fix.
The configured private root is `/tmp/raven-chrome-private-modules`, with
`chrome-multiblock-negative-0040`; no shared component was modified.

The supplied `multiblock_snapshot_client.c` was compiled with MinGW `-O2
-nostartfiles -Wl,-e,raven_entry -lkernel32` and run through
`scripts/test-fex-process.py`, with a 15-second timeout, against that root.
It returned zero without a repair. A second experimental version inserted
256 repetitions of nine arithmetic operations on EAX/ECX/EDX/EBX/ESI/EDI
before its pointer checks, declaring all those registers clobbered. It also
returned zero. That experimental change was reverted. Neither is a failing
regression, and neither establishes that multiblock snapshots are correct.

Inspection also ruled out the simple theory that branch instructions lack
RIP metadata: `CanHaveSideEffects` already includes FLAGS_SETS_RIP and
FLAGS_BLOCK_END. The prior Steam trace's snapshot at steamui.dll+0x3c005
corresponds to `addl 0x18(%rsp), %edx`, not a guest branch. This remains an
unresolved capture/reconstruction investigation, not a diagnosed root cause.

One paired harness run was attempted with one sample, using an APFS-cloned
private Chrome prefix. Another worktree was simultaneously running Raven
processes. Rosetta produced a checked DOM result (1734.2 ms JS, 31.901 s
process duration); the FEX lane was terminated without a DOM result when
available disk reached 6.7 GiB. These measurements are **not a valid
performance comparison**. Evidence: `/tmp/raven-mb-implementation-negative.json`
and its adjacent logs. Removing this run's disposable prefix restored 9.1
GiB. No new FEX modules were built, no Steam login was verified, and no
invalidation optimization or gain is claimed by this continuation.

Qualification of the supplied build (not a repaired multiblock build), all
through `/tmp/raven-ctest-lock.sh`, with `RAVEN_PREFIX_SETUP=1`:

- Area selection `-R 'fex|context|parity|steam|dxmt' -j4`: 560 tests,
  eight failures, two skips, 120.46 seconds. Six failures were missing
  context-groups/mtld3d-runtime fixture executables; these targets were built.
  The other failures were DXMT features and the multiblock-disabled contract.
- One full suite `-j4`: 1,597 registered, one disabled, 16 skipped,
  six failures, 217.30 seconds. DXMT features and the newly built fixtures
  passed. Four x87-sidecar tests lacked their fixture executables; those
  targets were then built. Remaining failures were shared-infinite-wait x32
  and the multiblock-disabled contract.
- One serial `--rerun-failed -j1`: 11/12 passed including cleanup; only
  `native_multiblock_disabled_fex` still failed, as expected for the supplied
  negative-control DLL. No further reruns were made.

Logs are `/tmp/raven-mb-implementation-{area,full,rerun}.log`.
`git diff --check` passed. Final observed available disk was 11 GiB.
The supplied uncommitted implementation files remain uncommitted and unchanged;
this continuation commits only investigation/qualification notes. Both requested
implementation goals and fresh Steam-login qualification remain outstanding.

## Steam multiblock experiment (2026-10-31, second continuation)

**Do not promote 0040 yet.** This experiment did not reproduce the old
SteamUI SHA fault, but multiblock still failed the anonymous-login acceptance
check. No register-reconstruction repair or deterministic regression was
produced, and the implementation task remains incomplete.

The source started at 09ed0d13 with its supplied uncommitted diagnostics,
0040 inverse patch, and private-module pins. The enabled configuration used
`build-perf/native/raven`, `RAVEN_PARALLEL=1`,
`RAVEN_FEX_MODULE_ROOT=/tmp/raven-chrome-private-modules`, and build id
`chrome-multiblock-negative-0040`. `/tmp/raven-steam-mb2` was an APFS clone
of `/tmp/raven-rosetta-steam-pristine`; Steam config/userdata in the clone
were removed and AutoLoginUser cleared using its reg.exe before launch.
No account was used and `/tmp/raven-steam-login-fex` was not accessed.

Three enabled launches were made, with fault tracing and the supplied
snapshot-ring diagnostic. The first was stopped during observation; the
second had a 180-second timeout; the third had a 360-second timeout. None
produced a sign-in surface or a `Sign in to Steam` CEF page. SharedJSContext
was present. The original SteamUI+0x3c98d SHA fault did not appear.

- Launch 1 faulted at `libcef.dll+0x3505d57`, guest `movq (%rcx), %rax`,
  with RCX=0. The actual faulting AArch64 instruction was `ldar x8,[x0]`
  at host PC 0x73e841f51c, with X0=0. Caller return address:
  `libcef.dll+0x34fbef3`.
- Launch 2 reached SharedJSContext first paint and timed out without a
  fault-trace access violation or login page.
- Launch 3 faulted at `libcef.dll+0x3524424`, guest `lock xaddl %edx,(%rcx)`,
  with RCX=0xefefefefefefefef and RDX=1. The actual AArch64 instruction
  was `ldaddal w6,w7,[x0]` at host PC 0x735e8c1be4, with X0 containing
  that same poison value. Caller return address: `libcef.dll+0x34fbf16`.
  The preceding guest instructions load the pointer from (%rdx), store it
  to (%rax), test it and branch before the atomic increment.

Guest disassembly used **cef.win64/libcef.dll**, not the different win7x64
binary. The host instructions above were independently disassembled from
pages dumped by the supplied diagnostic. They establish that the invalid
pointers were present at the native fault, before context reconstruction;
they do not establish where those pointers first became invalid or whether
a previous context restore caused it. A complete compiled-block dump and
root-cause diagnosis remain outstanding.

A single-block control used a separate pristine clone,
`/tmp/raven-mb2-singleblock-control`, with the same anonymous preparation,
the same source/runtime build except the host entry contract restored to
0x703c, and `/tmp/raven-steam-private-modules` (qualified 0029 DLL hash
fc6e04e4065da6559080c1f0189e69d50fa050c5e9d9cbb56d2c808f045817cd).
It reached the complete anonymous sign-in form within about 70 seconds.
Both its CEF title and 700x440 IOSurface readback were checked; username
and password fields were empty. It was then stopped without input.
The enabled contract and build were restored after this control.

Local evidence (not committed): `/tmp/raven-mb2-launch-{1,2,3}.log`,
`/tmp/raven-mb2-pages-{1,2,3}.json`,
`/tmp/raven-chrome-fault-18540-73e841f51c.bin`,
`/tmp/raven-chrome-fault-31203-735e8c1be4.bin`,
`/tmp/raven-mb2-jit-{18540,31203}.o`,
`/tmp/raven-mb2-control-steam.log`, `/tmp/raven-mb2-control-pages.json`,
and `/tmp/raven-mb2-control-capture/700x440.png`.

No Chrome or M5 timings were collected: the enabled configuration failed
its Steam gate, so there is no qualified before/after performance gain.
0040, the supplied diagnostics, pins and synthetic probe remain uncommitted.
No shared module root was modified, no debugger attached, and nothing pushed.

Qualification of the supplied enabled build (not a repair), all through
`/tmp/raven-ctest-lock.sh` with `RAVEN_PREFIX_SETUP=1`:

- Full build `cmake --build build-perf -j8` passed.
- Area selection `-R 'fex|context|parity|steam|dxmt' -j4`: 560 registered,
  one failure (`native_multiblock_disabled_fex`), two skips, 125.31 seconds.
  The disabled-state contract correctly rejects the supplied enabled DLL.
- One full suite `-j4`: 1,597 registered, one disabled, eight skipped,
  one failure (the same disabled-multiblock contract), 204.68 seconds.
- No serial rerun was needed: the sole failure is the deterministic contract
  disagreement already observed in the area run. Logs:
  `/tmp/raven-mb2-{area,full}.log`. `git diff --check` passed.
- The two disposable Steam prefixes and temporary control executables were
  removed by literal path after their process groups had stopped. Evidence
  logs, dumps and the anonymous control screenshot remain. Available disk
  stayed above 6 GiB; final observed free space was 17 GiB.

## Multiblock root-cause search (2026-11-01, third continuation)

**Not fixed; 0040 stays unpromoted.** No FEX patch was added, no module root was
rebuilt with `build-fex-modules.py`, and Steam was not launched (it would only
repeat the known failure). No benchmark gain is claimed.

TSO/ordering lead. Raven's FEX base (FEX-2609.1) has no memory-op IR passes at all:
only x87 stack optimisation, dead-flag elimination (DFE) and RA. So no load or store can
be elided or reordered across multiblock boundaries; TSO choice does not
depend on multiblock. libcef.dll has no `/volatileMetadata` (load-config
offset 0x100 is zero), so the ForceTSO per-instruction override cannot disable
TSO there. Both Steam faults are in one CEF function (caller
`libcef.dll+0x34fbea0`), whose `this` was already freed. Two of two faulting launches hitting the same
function argue against a random ordering race.

`multiblock_atomic_client.c` (committed, coverage only): four workers run a
`lock xadd` refcount loop with internal branches and plain publish/consume,
ending in `sub; cmp $0; jne` (also tried `test`/`and`). The controller
suspends them continuously. With 0040 it passed every run; >3,400 stops landed
exactly on the back-edge `jne`, with ZF consistent with R8. It is not a reproducer.

Headless Chrome (benchmark harness) also fails with multiblock: it timed out
at 240 s with "Network service crashed", where single-block finished in ~47 s.
Private one-sample bisects, built with `Darwin/build.py` into scratch outputs
and then deleted (0040 tree otherwise):

| variant | change | Chrome |
|---|---|---|
| A | no mid-multiblock doorbell checks (0022 CondJump + backward pending jump) | fail |
| B | DFE local only: successors read FLAG_ALL, no FoldBranch | fail |
| C | no call-return entry points (stop decoding at CALL) | pass once (117 s, one network crash) |
| D | multiblock forced off inside FEXCore (same RVA as 0040) | pass (46.7 s) |
| E | forward-only multiblock targets (call returns kept) | fail |

So neither Raven's back-edge doorbell nor cross-block DFE is the cause.
Call-return entries inside a multiblock (Raven's call-ret stack path, 0010/0011)
remain the leading suspect, but C's repeat runs were invalid (below).

**Harness became unusable.** After ~02:30 MST, control D failed on 5 of 5
later runs (fresh clones, a reused prefix and `TZ=UTC`): the browser exits in 6–25 s,
right after GPU init and before `MakeWindowAssociation`. The native engine still
passed (1522 ms JS). The raven binary, fixture and modules were unchanged; the cause is
unknown. Until the D control passes again, every Chrome result after that time
(C2/C3, X, R0040, RD*, W*) is invalid. Logs: `/tmp/raven-mb3-scratch/`.

Next steps: restore a passing D control first. Then repeat C at least 3 times; if it
holds, minimise to CALL + return-entry + `ret` with the call-ret stack under
suspension. An IR cross-block-use checker (variant X) was inconclusive: the
module's CRT log file never appeared.

## Fourth continuation (2026-11-01): harness and call-ret lead

build-perf was reconfigured and rebuilt against the restored raven-components.
The GPU path is still unreliable, but `--disable-gpu` works: the multiblock-off
control D passed headless `--dump-dom` with a checked result (49 s, JS 8535 ms).
Use `/tmp/raven-mb3-scratch/runc.sh MODDIR NAME` (fresh prefix clone,
`caffeinate -dims`, and a Sleep/DarkWake count for the run window).

From about 02:55 local the Mac keeps entering Maintenance Sleep and DarkWake on battery.
`caffeinate -dims` does not stop it. The repeat runs of variant C (C1/C2) took
620 s against a 200 s timeout, with 26–28 sleep events each, and are invalid.
No timed Chrome, Steam or benchmark result is valid until the machine is on AC
power and awake.

Leading hypothesis (not yet proven): stale call-ret stack returns into
invalidated multiblock code. In multiblock mode, CALL pushes (guest return RIP,
host address of the continuation inside the caller's multiblock). RET jumps
straight to that host address when the RIP matches, with no lookup. Single-block mode pushes
(0, 0), so RET always performs a lookup, which matches variant C passing.
`InvalidateThreadCachedCodeRange` clears a thread's call-ret stack only when
that thread's own `CachedCodePages` covers the range. A thread that entered
the block through linked branches or another thread's compile keeps its stale
entries. Ruled out: Raven's MEM_DECOMMIT+MEM_COMMIT returns zeroed pages on
both engines, so the clearing itself works.

Next: a deterministic test. Thread A compiles a multiblock caller F; thread B
enters F only through linked branches, then calls G, which rewrites F's
post-call bytes and flushes. The result depends on whether B returns into the
new or the old translation. If confirmed, fix in 0041 by clearing every
thread's call-ret stack on any invalidation (not only those with cached pages),
then qualify with variant C's removed.

`multiblock_callret_client.c` implements that test (coverage only). It returned 0
under single-block (D), 0040, variant C and the native engine. So this shape
does not reproduce a stale return. Thread B probably caches F through its own
lookup, which clears its call-ret stack. The call-ret hypothesis remains
unproven. Next variants: B enters F through a return (not a link); or F's
continuation lies on a different page from F's entry, with only that page
invalidated.

## Fifth continuation (2026-11-01): multiblock re-enabled with 0041

**Done: 0028 removed, multiblock on, 0041 added.** Worktree
`/Users/yount/.t3/worktrees/raven/t3-fe1ceb9d`, this branch merged with macos
73103efe (0050 and 0060–0063 included). Private module root:
`/tmp/raven-mbfix-modules`, build id `mbfix-0041`; xtajit64.dll
`4de30d05…`, RavenProcessInit RVA 0x705c; xtajit.dll `ac0ea9a6…`, WoW64 RVA
unchanged at 0x79cc. No shared module was touched.

Machine state: `pmset` reported **battery power** for every run here, not AC.
No run had a sleep or DarkWake event. Fix for runc.sh: its sleep count also
counted untimestamped `pmset -g log` lines (24 false events per run); count only
lines that start with a date.

### Root cause: stale multiblock call-ret entries (proven)

In multiblock mode a CALL pushes (return RIP, host address of the continuation
in the caller's block), and RET jumps straight to that host address when the
RIP matches. `InvalidateThreadCachedCodeRange` cleared a thread's call-ret stack
only if that thread's own `CachedCodePages` covered the range. A thread that
entered the block through a patched block link never caches it, so its stack
keeps the entry and RET returns into the old translation.

Why the old `multiblock_callret_client.c` passed: Raven multiplexes guest
threads onto resident FEX workers, and after a syscall a guest thread can resume
on another worker, which has a different call-ret stack. Its rewrite ran
VirtualProtect inside the callee, so the RET never reached the stale entry. The
rewritten client keeps A alive, so B gets its own worker. B's G spins without a
syscall while the main thread rewrites F. Results, `RAVEN_PARALLEL=1`:

| module | client exit |
|---|---|
| OFF (0028, single block) | 0 |
| MB (multiblock, no fix) | **21** (B returned the old immediate), 4/4 |
| G (MB, CALL always pushes zeros) | 0 (1 run) |
| FIX (MB + 0041) | 0, 4/4 |

A diagnostic build (MB plus invalidation records through
`RAVEN_FEX_PROFILE_COMPILE`) showed the gap directly. For the one invalidation
that dropped F from the shared cache, three workers had no own-cache hit, so
their stacks were left alone. Only the worker that compiled F was cleared.

0041 (FEXCore only): `GuestToHostMap::InvalidateRange` returns whether it
dropped code, `InvalidateCodeBuffersCodeRange` records this in
`CodeBuffersInvalidated` (under CodeInvalidationMutex), and
`InvalidateThreadCachedCodeRange` clears every thread's call-ret stack when it
is set. Threads with an own-cache hit are cleared as before. 0050–0063 apply
unchanged on top. Not covered: a thread already executing inside a block when
it is dropped can still push a new entry afterwards. That is a guest
cross-modifying-code race, and upstream behaves the same way.

The client is now ctest `native_multiblock_callret_fex` (fex;process,
RAVEN_PARALLEL=1). It replaces `native_multiblock_disabled_fex` and
`scripts/test-fex-multiblock.py`, which byte-matched 0028's
`Config::Set(MULTIBLOCK, "0")` call.

### Chrome and Steam

The earlier multiblock Chrome/Steam failures **no longer reproduce on today's
base, even without 0041**. The Chrome harness gave these results (fresh clone,
headless `--dump-dom`; modules use `CONFIG_MULTIBLOCK "1"` instead of deleting
the line, so they keep RVA 0x7070 and share one raven):

- `--disable-gpu`: OFF 2/2, MB 5/5, C 3/3, G 3/3, FIX 5/5 pass.
- GPU on: OFF 2/2, MB 3/3, FIX 1/1 pass.
- Final 0041 modules: 2/2 GPU on, 1/1 `--disable-gpu` pass.

The old GPU-on and Steam multiblock failures were on the pre-merge base. The
`--disable-gpu` multiblock runs then were all sleep-invalidated. So the earlier
failure probably went away with today's macos FEX fixes (01132ec4 cache-flush
forwarding / retained translations, aa1f113e shared-view validation). That is
not bisected. The call-ret defect is real, but no workload here showed it.

Steam: fresh `cp -cR` of `/tmp/raven-rosetta-steam-pristine`,
loginusers.vdf, config.vdf and userdata removed, AutoLoginUser cleared,
`RAVEN_PARALLEL=1`, `-cef-enable-debugging`, final 0041 modules: CEF reported
"Sign in to Steam" after 28, 28 and 29 s (3/3). Nine x64 FEX processes used
RVA 0x705c and none had an access violation. Script:
`/tmp/raven-mbfix-scratch/steam.py`.

### Benchmark (`benchmark-chrome-fex.py --samples 3`, fresh clone of /tmp/raven-chrome-fex)

| configuration | JS ms (mean) | wall s (mean) |
|---|---|---|
| FEX, multiblock off (0028 module, `8a57bc46…`) | 4730 / 4835 / 4674 (4746) | 31.0 |
| FEX, multiblock + 0041 semantics (RVA-0x7070 variant) | 4614 / 4536 / 4560 (4570) | 24.1 |
| FEX, final 0041 modules (`4de30d05…`) | 4504 / 4477 / 4484 (4488) | 24.5 |
| Rosetta (native engine) | 1148 / 840 / 1148 (1045) | 19.1 |

Multiblock cuts the JS time ~5% and Chrome's whole launch-to-DOM wall time
~21%. The off/on rows used the same raven binary built at the 0x7070 contract.
All runs were on battery, so the numbers are indicative only. Evidence:
`/tmp/raven-mbfix-scratch/bench-{off,on,final}.json`.

### Tests

- Targeted: `native_multiblock_callret_fex` passed (run without the ctest
  lock by mistake; 2 tests, 4 s).
- FEX area, `RAVEN_PREFIX_SETUP=1`, through `/tmp/raven-ctest-lock.sh mb-fix`,
  `-R 'fex|flush|retranslation|context|parity|thread|jit|selfmodify' -j8`:
  554 tests: 6 failed, the other 548 passed or skipped (2 skips, mtld3d32/64). The six failed in <0.1 s:
  context-groups32/64 native+fex and mtld3d-runtime32/64 fex lacked fixture
  executables that are not in ALL.
- After building those four fixtures, one serial `--rerun-failed -j1`:
  12/12 passed. Logs: `/tmp/raven-mbfix-scratch/{area,rerun}.log`.
