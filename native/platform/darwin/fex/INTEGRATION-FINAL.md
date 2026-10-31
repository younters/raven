# Combined FEX integration verification — 2026-10-31

One full suite was run from clean `c60a06b` using the supplied modules (patches
0001–0018). The previous disk-full attempt is excluded. Every CTest invocation,
including baseline and focused rechecks, used `/tmp/raven-ctest-lock.sh`.
The missing wrapper was recreated with an exclusive flock on the existing lock
directory; no other CTest process was active. The supplied module root was not
modified. Disk checks on `/System/Volumes/Data` showed 23 GiB initially, a minimum
observed 14 GiB, and 15 GiB at completion. No ENOSPC or PE backing-file failures
occurred. No debugger, push, or edit to macos/other worktrees/shared components
was used.

## Reproduction and build controls

Ordinary fixtures use `RAVEN_PREFIX_SETUP=1` to bypass real prefix COM
registration. Registration tests **unset** it, remove parallel scheduling, use
fresh temporary prefixes, and require the registration marker including
windowscodecs.dll. Real application launches also unset it. Do not interpret
ordinary fixture passes as registration evidence.

```sh
RAVEN_PREFIX_SETUP=1 RAVEN_FEX_MODULE_ROOT=/tmp/raven-integ-modules \
  /tmp/raven-ctest-lock.sh integration-final \
  ctest --test-dir build-integration -j8 --output-on-failure \
  --output-junit /tmp/raven-final-ctest.xml
```

Initial build-integration had an empty CMAKE_BUILD_TYPE and a private RTL DLL.
The current macos baseline is `051344d306ef7dba5e1da1454f69049cbb9a1adc`,
RelWithDebInfo, with the provisioned RTL. A read-only Ninja dependency dry run
of all baseline targets reported no work. A temporary manifest renamed only its
regeneration edge to avoid dry-run glob regeneration masking target freshness.
Baseline checks used baseline executables/fixtures where available, the current
harness, and writable output/prefix locations in this worktree. The new strict
context fixture used this branch's fixture against the baseline runtime.
Baseline sources and build were not changed.

The branch was rebuilt as RelWithDebInfo with the same RTL as macos. This
resolved five timeout failures and the private-RTL RPC fault without source
changes. The two intermittent failures also passed serially.

## Results and every failure classification

Full run: **727 registered: 692 passed, 28 failed, 6 skipped, 1 disabled**,
281.53 seconds. This is not a green full-suite qualification.

Final focused run after the fixes and matching baseline build/component inputs:
**209 selected: 190 passed, 19 failed, zero skips**, 117.37 seconds. It includes
all 28 original failures, all test names containing fex, and the four registration
lanes. Every remaining failure also reproduced on current macos. No unresolved
regression remains among these checks. The full suite was not repeated after
fixes; the two runs must not be combined into a synthetic full-suite pass count.

| Original full-suite failure | Classification |
| --- | --- |
| `darwin_dcomp_foreign` | Pre-existing |
| `darwin_dcomp_foreign-hidpi` | Pre-existing |
| `darwin_dcomp_local` | Pre-existing |
| `darwin_dcomp_local-hidpi` | Pre-existing |
| `darwin_dcomp_popup` | Pre-existing |
| `darwin_dcomp_popup-hidpi` | Pre-existing |
| `darwin_dxmt_device10` | Pre-existing |
| `darwin_dxmt_fullscreen` | Pre-existing |
| `darwin_dxmt_pacing` | Pre-existing |
| `darwin_dxmt_present_rate` | Pre-existing |
| `darwin_dxmt_window` | Pre-existing |
| `darwin_x87sidecar_child` | Environmental: intermittent |
| `native_cmd_x32` | Environmental: build configuration |
| `native_darwin_cocoa_input` | Environmental: intermittent |
| `native_darwin_desktop_dpi_x32` | Environmental: build configuration |
| `native_dwrite` | Pre-existing |
| `native_handle_channel_contract` | Regression: fixed |
| `native_parallel_lifecycle_x64` | Pre-existing |
| `native_parity_context_native` | Pre-existing |
| `native_pipe_roundtrip_x32` | Environmental: build configuration |
| `native_rpc_auth_x64` | Environmental: component mismatch |
| `native_thread_snapshot_x32` | Environmental: build configuration |
| `native_twinapi_x32` | Pre-existing |
| `native_twinapi_x64` | Pre-existing |
| `native_x86_dwrite` | Pre-existing |
| `native_x86_registry` | Environmental: build configuration |
| `native_xmllite_x32` | Pre-existing |
| `native_xmllite_x64` | Pre-existing |

The six skips are Vulkan descriptor-buffer x64/x32, Minesweeper workflow,
Darwin authorized capture, installer WIDL, and fd-budget low-hard. The disabled
test is native_vulkan_queued_extensions_x64. Full output for each failure and
baseline/final comparisons is in [results.json](integration-final/results.json).

## Fixes found by verification

- Child spawning still mapped the child image solely to decide a width argument
  that became unused once both FEX widths were supported. This rejected the
  handle-channel unit's synthetic ipc-unit.exe. Remove that obsolete preflight;
  existing handle-channel and FEX child x32/x64 tests pass.
- Fresh FEX Notepad++ exited 138 before an editor. Signal tracing (no debugger)
  identified a native LookupCache::AddBlockMapping write to a decommitted
  overcommit page. Patch 0019 lets executable x64 module code repair only
  tracker-owned overcommit ranges, retaining the Mach-O/non-module rejection
  boundary. This composes with JIT guard repair and leaves guest reconstruction
  gated on JIT/dispatcher PCs. Notepad++ now reaches its editor.

The supplied modules were cloned to `/tmp/raven-final-modules`, then rebuilt
with patches 0001–0019, build id integration-final19. Missing toolchain wrapper
files in that private clone were restored from the authenticated archive.
The cached driver then accepted the exact source tree, hashes, audits, and
native-host preflight. Pins and the native ProcessInit RVA were updated.

```sh
python3 scripts/build-fex-modules.py --raven-host --build-only \
  --root /tmp/raven-final-modules --build-id integration-final19
cmake -S . -B build-integration -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRAVEN_RTL_COMPONENT_PATH=/Users/yount/Documents/Projects/raven-components/rtl/raven-rtl.dll \
  -DRAVEN_FEX_MODULE_ROOT=/tmp/raven-final-modules
cmake --build build-integration -j8
# All subsequent CTest selections still go through /tmp/raven-ctest-lock.sh.
```

## Fresh-prefix registration

Times are the test script's registration_ms wall times, including setup,
launch and marker verification, with one sample per cell. These are observations
on a shared machine, not a controlled performance benchmark.

| Build and width | Rosetta | FEX |
| --- | ---: | ---: |
| Initial unoptimized x32 | 20.4042 s | 23.5620 s |
| Initial unoptimized x64 | 16.9299 s | 20.0546 s |
| Final RelWithDebInfo x32 | 3.6277 s | 3.6162 s |
| Final RelWithDebInfo x64 | 3.4114 s | 3.4047 s |

The earlier approximately 24.5-second observation is consistent with the
unoptimized lane; the final build/component configuration differs. These data
do not justify making FEX the default.

## Notepad++ editor verification

Both final launches used genuinely absent prefixes, real registration,
`--execution=ENGINE --timeout-ms=90000 --desktop=macos
--directory=/tmp/raven-review-npp/npp /tmp/raven-review-npp/npp/notepad++.exe`,
with --run-process first. The missing fixture was restored by cloning the local
portable fixture to the requested temporary path. Prefixes were removed only
after each owned process group was terminated. Final launch-to-editor observations
were Rosetta **5.580 s**, FEX **6.112 s**, excluding the two-second painting delay.
Only editor visibility is qualified, not editing or clean application exit.
Both final captures were visually inspected: title, menus, toolbar, new document
tab, line 1, editor canvas, and status bar are visible.

- [Rosetta editor](integration-final/npp-native.png), [log](integration-final/npp-native.log)
- [FEX editor](integration-final/npp-fex.png), [log](integration-final/npp-fex.log)

A prior Rosetta window-capture attempt failed transiently; the final fresh-prefix
retry captured successfully. Earlier FEX failures and all initial suite results
remain in `/tmp/raven-final-*.log`; machine-readable test results are
`/tmp/raven-final-{ctest,baseline,recheck,optimized,child-fix,rtl,qualified}.xml`.
