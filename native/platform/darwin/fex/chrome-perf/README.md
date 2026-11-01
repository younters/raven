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
