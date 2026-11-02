# FEX code cache: results (2026-11-01)

Branch `perf/fex-code-cache`. Private module root `/tmp/raven-codecache-modules`,
pinned final build `codecache-0064f` (xtajit64.dll `13cd2b91…`). Chrome cold/warm
used `codecache-0064d` (`53fce6d5…`) and Steam used `codecache-0064e`. The later
builds change only the exit flush bound (0064d: 5000 one-millisecond polls; 0064e:
1000; 0064f: 3 s wall clock). Contract RVAs are unchanged (0x705c / 0x79cc). Raven
`build-cc`, RelWithDebInfo, options copied from the reference build-macos cache.
The Mac was on AC power for every run, with no sleep or DarkWake event between 17:12
and 18:10 (`pmset -g log`). mediaanalysisd was using ~110% CPU in the background
throughout, so wall times are shared-machine numbers.

## Chrome JS harness

`scripts/benchmark-chrome-fex.py --samples 3`, a fresh `cp -cR` of
`/tmp/raven-chrome-fex` per variant, headless with GPU, `RAVEN_PARALLEL=1`.
Cold = a new empty cache directory per sample; warm = the cache left by the third
cold sample. All DOM checks passed.

| configuration | wall s | wall mean / median | JS ms | JS mean |
|---|---|---|---|---|
| Rosetta (native) | 9.2 / 9.2 / 7.2 | 8.5 / 9.2 | 1177 / 1181 / 1033 | 1130 |
| FEX, cache off | 28.3 / 28.3 / 27.8 | 28.1 / 28.3 | 4656 / 4636 / 4596 | 4629 |
| FEX, cache cold | 18.1 / 36.7 / 46.0 | 33.6 / 36.7 | 4730 / 4744 / 4543 | 4672 |
| FEX, cache warm | 53.1 / 22.4 / 22.5 | 32.6 / 22.5 | 4272 / 4268 / 4148 | 4230 |

Two further warm launches with a diagnostic timeline (same cache, one build later
with only extra prints) took 15.1 s and 24.0 s, with JS 4489 and 4449 ms.

- **JS:** warm is 8.6% faster than off (4230 vs 4629 ms). Cold matches off.
  FEX is still 3.7x Rosetta.
- **Translation time** (`RAVEN_FEX_PROFILE_COMPILE`, one launch each): time spent in
  `CompileBlock` across all processes fell from 10.35 s (off) to 4.98 s (warm), and
  in the browser process from 7.48 s to 3.12 s. That is about 41 µs per cache hit
  against about 105 µs per compile.
- **Hit rate:** a warm launch hits about 99.5% of lookups (for example, browser
  75,493 hits and 237 misses). A cold launch also hits 24k blocks inside the browser:
  blocks retranslated after invalidation come back from the cache.
- **Wall time is bimodal and not a reliable metric here.** A timestamped off launch
  shows a ~10 s window, before the DOM result, in which no process starts. Off has
  exactly one such window in every launch (all ~28 s). Cache launches have zero
  (15–22 s), one (~24 s) or two (36–53 s) of them. No run logged a crash or fault.
  The cause of the ~10 s window was not found; it exists without the cache. So
  only the median (22.5 vs 28.3 s) suggests a launch gain, and that is not
  established.
- **Size:** one cold launch writes about 780 MB (about 122k records, ~23%
  duplicates from processes compiling the same blocks at once). Median record
  2 KB; the 1% of records over 114 KB (large multiblocks) hold about 30% of the bytes.

Superseded runs, kept only as history: cold/warm samples taken before the torn-tail
fix discarded the cache mid-run when a killed writer left a torn index record. Their
warm run was stopped and is not reported. The off/Rosetta row comes from the build
before the last two module changes. Both changes only touch cache-enabled paths:
torn-tail handling and the exit flush bound.

## Steam to the sign-in screen

`/tmp/raven-codecache-scratch/steam.py`: a fresh `cp -cR` of
`/tmp/raven-rosetta-steam-pristine`, anonymous (`loginusers.vdf`, `config.vdf` and
`userdata` removed, AutoLoginUser cleared), `RAVEN_PARALLEL=1`,
`-cef-enable-debugging`. Pass = CEF reports "Sign in to Steam". Polled every second.
Final modules (`codecache-0064e`).

| configuration | seconds |
|---|---|
| cache off | 22.8 / 22.7 |
| cache cold | 23.0 |
| cache warm | 19.5 / 20.4 |

Warm is about 12% faster. The warm processes that exited normally hit 7.3k–7.6k blocks
with no rejection. The Steam cache is 354 MB after one cold launch.

## Correctness

`scripts/test-fex-code-cache.py`, ctests `native_code_cache_fex_0/1` (serial and
`RAVEN_PARALLEL=1`):

- baseline, cold and warm results are identical; warm hits every stored block;
- a same-named DLL with the same size and timestamp but different bytes computes its
  own result, and its 11 blocks miss;
- flipped bytes in blobs are rejected by the integrity hash (65 of 65);
- a truncated main file is detected as damaged, set aside, and rebuilt;
- a torn index tail keeps earlier records, and records appended after it are reached;
- a garbage index is rebuilt; a bad header disables the cache for one run, then
  the cache is rebuilt;
- two processes populating one cache concurrently, then a third hits every block;
- JIT code and self-modified image code never appear in the database; unmodified
  image code does (positive control).

Test runs, each through `/tmp/raven-ctest-lock.sh` with `RAVEN_PREFIX_SETUP=1`:

- One targeted run (`code_cache|multiblock_callret|retranslation|alias_flush|flush-only|modhost|selfmodify`,
  -j4): 71 of 73 passed. Both code-cache tests failed one assertion: after a torn
  tail, the next run reached only some of the 11 records appended after it.
  Cause: the exit flush (then about 1 s) dropped queued stores under load. The
  trim itself worked, since hits rose. The flush now has a 3 s wall-clock bound, and the
  assertion checks that hits rise, which fails if the trim is missing.
- One FEX-area run (`fex|flush|retranslation|context|parity|thread|jit|selfmodify|multiblock`,
  -j8) after the fix: 558/558 passed, 2 skipped (mtld3d32/64 have no overlay root),
  89 s. No serial rerun was needed.

At about 18:10, while these were being prepared, something outside this work purged
old-access files under `/tmp`: `/tmp/raven-chrome-fex` shrank from 1.0 GB to
532 MB and the toolchain copy in the private module root lost files. All Chrome and
Steam timings above finished before 18:10. The toolchain was re-extracted from its
pinned, checksummed archive before the final module build.
