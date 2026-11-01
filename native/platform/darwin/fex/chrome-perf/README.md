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
