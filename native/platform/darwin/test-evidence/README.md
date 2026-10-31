# macOS suite disk use and failure investigation — 2026-10-31

Source baseline: `e4d388d7b66845452bf324f960f73cf3e5be7aa4`.
Branch: `fix/macos-test-disk-and-failures`.

## Build and measurement

An isolated Ninja RelWithDebInfo build was configured in this worktree's `build-macos`. FONT_HASHES, both PNG paths, every RAVEN component path (including SQLite), graphics overlays, fonts, roots, Skia paths, and the hidden-surface option were copied from the original build cache. FEX modhost is ON and uses `/Users/yount/Documents/Projects/raven-components/fex-modules`. Builds used `-j8`; all 269 targets ending in `_fixture` or `_fixtures` were explicitly built.

Every CTest invocation used `/tmp/raven-ctest-lock.sh`. `monitor.py` is the exact measurement harness used: it samples volume free space every 100 ms and records open unlinked files with `lsof +L1`. It starts a dedicated process group and stops only that run if free space drops below 10 GiB, leaving a margin above the requested 8 GiB. No debugger was attached and no unrelated processes were killed.

Example invocation (from the worktree):

```sh
/tmp/raven-ctest-lock.sh impl-before python3 native/platform/darwin/test-evidence/monitor.py before env RAVEN_PREFIX_SETUP=1 ctest --test-dir build-macos -j8 --output-on-failure
```

Raw timestamped samples and summaries are adjacent to this report. Peak means the largest observed decrease in **whole-volume free space**, not a sum of logical file lengths. Background activity on the shared machine can affect that measurement.

| Run | Peak extra space | Minimum free | Functional tests |
| --- | ---: | ---: | --- |
| Unmodified baseline | 13.12 GiB | 12.62 GiB | 742: 715 pass, 20 fail, 6 skip, 1 disabled |
| After disk fix (lifecycle checker also updated) | 1.03 GiB | 20.80 GiB | 742: 719 pass, 16 fail, 6 skip, 1 disabled |
| Final fixed suite | 0.30 GiB | 19.28 GiB | 742: 735 pass, 0 fail, 6 skip, 1 disabled |

The post-fix run also passed 677 prefix cleanup fixtures. These are housekeeping tests, not new functional coverage. The previously reported 25–30 GiB exhaustion was not reproduced to that extent; this investigation never approached ENOSPC.

## Disk root cause and fix

`NativePrefix::place_runtime_components` physically copied runtime DLLs into System32 and SysWOW64 for each prefix. CTest assigned prefixes both under `native/test-prefixes` and in individually named build directories, but never removed them. Examples include native_cmd, native_cmd_x32, native_system_cli and the many x86 library clients: each populated prefix retained roughly 24–28 MiB. The original build retained 6.6 GiB in `test-prefixes` alone. During the new baseline that directory reached 6,096,072 KiB; the entire build grew from about 2.2 GiB to 12,199,032 KiB, including other prefixes and test output. This is cumulative suite-wide duplication, not one giant-file test.

During the baseline, `/cores` remained empty, swap stayed at 2 GiB, and the largest observed unlinked Raven backing files were approximately 49 MiB. A search found no generated file larger than 500 MiB. The readable Rosetta cache was about 82 MiB after testing. Snapshot inventory showed OS-update snapshots, not newly created test snapshots. No evidence identified these as the dominant source of the measured growth; the prefix changes alone reduced the full-suite peak to 1.03 GiB.

On Darwin, runtime placement now uses `copyfile(..., COPYFILE_CLONE)` (APFS clonefile semantics, with copying fallback for other filesystems). Prefix files remain independently writable; there are no hard links sharing writable DLL contents. Existing up-to-date components are preserved.

CTest cleanup fixtures delete each build-owned prefix after its last consumer, on success, failure or timeout. Tests sharing a prefix share its cleanup fixture, and filtered runs automatically include cleanup. Caller-owned prefixes outside the build directory are untouched. `test-prefixes` was empty after the post-fix suite. No test assertions or workloads were removed to reduce disk use.

## Failure classification after the disk fix

Every failure in the required post-disk full run is accounted for here:

| Tests | Classification and evidence |
| --- | --- |
| darwin_dcomp_local, darwin_dcomp_foreign, darwin_dcomp_local-hidpi, darwin_dcomp_foreign-hidpi, darwin_dcomp_popup, darwin_dcomp_popup-hidpi | Real bug: `present → place_host_surface → place_hosted → present` recursed and exhausted the scheduler stack, surfacing as AppKit heap/stack corruption. Placement now avoids repainting an already-painted root. All six pixel tests passed the targeted recheck. A visible hosted-child regression also passed ten repeats. |
| darwin_dxmt_device10, darwin_dxmt_fullscreen, darwin_dxmt_pacing, darwin_dxmt_present_rate, darwin_dxmt_window | Real runtime failures, not ENOSPC: host SIGTRAP/SIGSEGV while presenting. The hosted-surface recursion is shared with dcomp. No `darwin_dxmt_*` test or DXMT implementation was changed; implementation work remains with the other agent. |
| native_parity_context_native | Real API bug: native GetThreadContext returned stale state for a running remote thread and claimed success for unsupported register groups. Native validation now rejects those requests, matching FEX. The old native context fixture's expected rejection code was updated to the unified ERROR_INVALID_PARAMETER contract. Native/FEX parity and the native redirect-context test passed ten repeats. |
| native_pipe_roundtrip_x32 | Test deadline bug under load: its unchanged 10,000 round trips inherited Raven's five-second execution limit despite a 30-second CTest budget. Explicit guest timeout is now 25 seconds. Both widths retain all transfers and the p99 latency assertion, and passed the targeted recheck. |
| native_provider_contract, native_resource_contract | Intermittent: passed the unmodified full suite, failed the post-disk suite, then passed the targeted parallel rerun. No deterministic root cause established; do not attribute these to ENOSPC. |
| fex_modhost_substrate | Real startup race, also responsible for intermittent fex_modhost_execution failures. A worker could call NtTerminateThread before pthread_create wrote the worker ID into shared control state, causing rejection and the fixture's explicit abort. The worker now stores pthread_self before publishing readiness. Both tests passed ten repeats, exercising 64 self-terminating workers per substrate execution. |

## Other entries from the supplied failure list

| Tests | Classification |
| --- | --- |
| native_parallel_lifecycle_x64 | Real test expectation bug: x64 routes the intentional worker access violation through Windows SEH (status c0000005, host exit 5), while the checker expected the old host signal. An explicit `--seh` mode requires the correct exit and exception diagnostic. Process exit, timeout, sampling and x32 fault checks are retained. Both widths passed. |
| native_pipe_roundtrip_x64 | Same test deadline issue as x32; passed the post-disk suite and targeted check. |
| fex_modhost_execution | Same confirmed worker-startup race as substrate, fixed and repeatedly verified. |
| native_bcrypt, native_accprop_x32, native_crt_errors_x32, native_global_stream_x32, native_integrity_elevated_x32, native_integrity_limited_x32, native_object_name_x32, native_shell_info_x64 | Environmental/intermittent: passed the post-disk full suite. The original ENOSPC run is insufficient to establish an independent code defect. |
| native_fresh_prefix_x32_fex, native_fresh_prefix_x64_fex, native_fresh_prefix_x32_native, native_fresh_prefix_x64_native | Environmental/intermittent: all four passed with real fresh-prefix COM registration after the disk fix. No registration checks bypassed. |
| native_darwin_capture_authorized | Environmental skip: screen-capture authorization prerequisite. |
| native_darwin_fd_budget_low_hard | Environmental skip: host hard-limit prerequisite unavailable. |
| native_darwin_installer_widl | Known external dependency: WIDL unavailable. |
| native_minesweeper_workflow | Known external dependency: user-supplied RAVEN_MINESWEEPER executable absent. |
| native_vulkan_descriptor_buffer_x32, native_vulkan_descriptor_buffer_x64 | Known external graphics capability limitation; explicit skips. |
| native_vulkan_queued_extensions_x64 | Existing disabled graphics capability test, not an executed failure. |

Additional unmodified-baseline failures in native_kernel_memory_x32_default_model, native_kernel_memory_x32_default_small, native_kernel_threads_x32_default and native_bcrypt_x32 did not recur in the post-disk suite: intermittent, with no demonstrated ENOSPC involvement in these measured runs.

## Final verification

Final full suite at code commit `d0834a71` completed in 148 seconds with **zero failures**: 735 functional passes, 6 explicit skips and 1 disabled test, plus 677 successful cleanup fixtures (1419 CTest entries total). The final measured peak extra disk was 317,325,312 bytes (0.30 GiB), with minimum free space 20,697,772,032 bytes (19.28 GiB).

All six dcomp tests, both lifecycle widths, native/FEX context parity, both pipe roundtrips, both modhost tests, and both previously intermittent provider/resource tests passed. The five original DXMT failures also passed after the shared hosted-surface fix, without edits to DXMT implementation or its tests. The six skipped and one disabled tests retain the classifications above.

No pushes were made. Only this worktree was changed. Logical commits:

- `9f5c073f`: APFS clones and prefix cleanup.
- `b51abad5`: lifecycle SEH expectation and pipe test execution deadline.
- `74fbc7b5`: hosted-surface recursion and regression test.
- `188c1616`: native context validation and error contract.
- `d0834a71`: modhost worker identity publication race.

The following evidence-only commit records this report and the measurements.
