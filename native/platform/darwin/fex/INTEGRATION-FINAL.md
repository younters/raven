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
RAVEN_PREFIX_SETUP=1 RAVEN_FEX_MODULE_ROOT=/tmp/raven-review-modules \
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
  --root /tmp/raven-review-modules --build-id integration-review
cmake -S . -B build-integration -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRAVEN_RTL_COMPONENT_PATH=/Users/yount/Documents/Projects/raven-components/rtl/raven-rtl.dll \
  -DRAVEN_FEX_MODULE_ROOT=/tmp/raven-review-modules
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
are local-only, uncommitted evidence in `/tmp/raven-final-*.log`; the likewise
local-only machine-readable test results are
`/tmp/raven-final-{ctest,baseline,recheck,optimized,child-fix,rtl,qualified}.xml`.

## Review fixes and final-tree qualification (2026-10-31)

The review build uses the **private** clone `/tmp/raven-review-modules`, build id
`integration-review`. The supplied `/tmp/raven-final-modules`, shared FEX trees,
and other worktrees were not changed. Patch 0019 now also exports Raven-host-only
qualification probes for its real overcommit tracker and CRT allocator. The
cached build driver passed source identity, patch/module hashes, audits, and
native-host preflight. RelWithDebInfo and the provisioned RTL above were retained.

| Finding | Disposition and evidence |
| --- | --- |
| C1 | Both callbacks share a null-safe helper. Fixed per-pthread fallback TEBs allocate nothing and have distinct TlsSlots. Both width boundary tests exercise two simultaneous null-active-thread callers and restoration to the active environment. |
| C2 | No defect reproduced: each width alternates two guest TEBs 100 times on one resident worker under `RAVEN_PARALLEL=1`. Every GS/FS self-pointer load matches the selected TEB. No segment-refresh bit was restored. |
| C3 | Owner-thread destruction is enforced before teardown; failed x64 ThreadTerm terminates with a diagnostic. Destructors are noexcept. WoW64 BTCpuThreadTerm is void, so its register return is not interpreted as NTSTATUS. |
| C4 | Parsed the built xtajit.dll export table: RavenWow64ProcessInit remains RVA `0x7850`, entry word `0xf81f0ffe`. Its SHA-256 is unchanged. The native x64 redirection still resolves to `0x6cbc`, word `0xb40001c0`. |
| T1 | `native_module_regressions_fex` writes and verifies `0x123456789abcdef0` from module code into an actual tracker-owned reservation. `native_reserved_write_rejection_fex` performs a real Mach-O write to reserved memory in an isolated executable and requires unsupported-fault exit 138. A synthetic non-module PC is also rejected by RavenHandleFault. |
| T2 | The module regression lane allocates 256 CRT blocks on A, frees them on B, and repeats 64 rounds while alternating A's execution TEB. B has its own initialized private TEB and is finalized on B. CTest imposes a 30-second deadline on allocator spins. |
| T3 | Restored instruction+1 rejection; added non-executable syscall, wrong third byte, and inaccessible third-byte page rejection before valid service completion. Cooperative and parallel NT-service lanes pass. |
| T4 | Marker checks require msxml3.dll, windowscodecs.dll, mlang.dll and propsys.dll for both component widths. A new PE32 fixture verifies the loaded codec's module path contains SysWOW64. The subprocess has a clear 105-second error, inside CTest's 120-second deadline; its nested runner has a 100-second deadline. |
| T5 | PASS is printed only after valid registration. Removed PASS_REGULAR_EXPRESSION so exit status remains authoritative, added registration-failure rejection and SKIP_RETURN_CODE 77. |
| T6 | Fixture wait 30 s, Raven deadline 45 s, runner deadline 55 s, CTest deadline 75 s. All eight jit stress lanes pass. |
| T7 | Native-engine TLS-slot and jit-stress lanes at both widths/scheduling modes, and both native_fex_child widths, have recorded passing runs. |
| D1 | Corrected series to 0001–0019. |
| D2 | DEFAULT/M4/M5 now explicitly distinguish the initial 0018 full run from the 0019 focused rerun. This section records the later final-tree full run separately. |
| D3 | Both integration notes specify the provisioned shared RTL DLL selected in the build cache; historical private RTL inputs are identified as historical. |
| D4 | Reproduction commands now select the current private module root and build id. |
| D5 | Historical /tmp logs/XML are explicitly local-only; review result excerpts below are committed. |
| D6 | Converted both C fixtures to tabs, wrapped parity properties/commands, factored repeated fixture compilation, and documented the legacy SDK requirement for x18. |

Current module SHA-256:

- xtajit64.dll: `703ee6a07b7f612941447272cccad9a8487b8dfa4e1e5c54d434fa643a6504bf`
- xtajit.dll (unchanged): `b84bca9f2b7e8797c46c51729149c4c8dfeb35b5b230960635f5493929df45d9`

Every review CTest invocation used `/tmp/raven-ctest-lock.sh` with
`RAVEN_PREFIX_SETUP=1` and `RAVEN_FEX_MODULE_ROOT=/tmp/raven-review-modules`.
The registration lanes explicitly unset prefix setup internally.
The final focused selection (`-R 'fex|tls|jit|fresh_prefix' -j8`) ran **197 tests:
196 passed, 1 failed**, 19.83 seconds. The failure was x64 FEX fresh registration:
`PE preferred base is unavailable and image has no relocation table`; the fixture
itself returned zero, but a registration child failed and no marker was accepted.
A subsequent locked serial registration selection passed **all four lanes** in
13.76 seconds. An earlier focused child-x64 address collision likewise passed its
serial recheck and the final focused selection. These address-placement failures
are recorded as intermittent, not hidden by automatic retries in the tests.

The **one final-tree full suite** ran **731 registered: 699 passed, 25 failed,
6 skipped, 1 disabled**, in **99.56 seconds**. It is not a green qualification.
All 19 previously classified failures remain. All four new CTest lanes, every
TLS-slot/jit-stress engine/width/scheduling lane, both FEX child widths, and all
four fresh-prefix lanes passed in this full run. Four additional full-suite
failures passed the locked serial recheck; two additional failures were compared
against existing baseline executables without modifying the baseline build:

| Additional full-suite failure | Classification/evidence |
| --- | --- |
| native_pipe_roundtrip_x32 | Intermittent 5-second timeout; serial pass in 2.69 s. Previously seen as an environmental failure in the initial integration run. |
| native_darwin_parallel_contracts | Intermittent scheduler wake failure; serial pass in 0.14 s. |
| fex_modhost_substrate | Intermittent abort during full-suite concurrency; final focused and serial selections pass. Exact contention cause not established. |
| fex_modhost_execution | Intermittent abort during full-suite concurrency; final focused and serial selections pass. Exact contention cause not established. |
| native_fiber_x64 | Pre-existing native IsThreadAFiber implementation gap: existing macos raven with this branch's fixture reports the identical unimplemented-API failure. |
| native_compatible_bitmap_contract | Bootstrap-input-dependent: baseline host/current bootstrap fails with the same DIB-release assertion; current host/baseline bootstrap passes. Baseline/baseline also passes. Bootstrap sources and build settings were not changed by this review; the exact agent cause remains unqualified. |

These are classifications of observed failures, not a claim that every
full-suite test now passes. Baseline cross-checks ran through a temporary CTest
manifest under `/tmp/raven-review-baseline`, with baseline executables read-only
and writable prefixes/logs outside the baseline worktree. The serial selection
had **6 tests: 4 passed, 2 failed**. The final full suite was not repeated.
Committed [review-results.json](integration-final/review-results.json) contains
trimmed failure output, summaries, and individual review-lane pass records.
Full untrimmed `/tmp/raven-review-*.log` and `.xml` files are local-only.
Disk remained at least **14 GiB** free on `/System/Volumes/Data` at observed
checkpoints (15 GiB initially). No shared components or other worktrees were
modified, no debugger attached, and nothing was pushed.


## Compatible bitmap allocation regression repair (2026-10-31)

This supersedes the `native_compatible_bitmap_contract` classification above.
The original integration bootstrap failed the unchanged test immediately in a
locked reproduction (0.06 s). The defect is in the Rosetta agent's high-address
`range_free` implementation, not GDI resource deletion or FEX memory accounting.
It used `mmap(hint)` followed by `munmap` as an availability query. That query
mutates the address space and executes Rosetta's mapping machinery while trying
to establish that the just-released address is free.

Temporary instrumentation (removed from the final patch) observed a failing
query for `0x1089a4000`, length `0x1000`, returning probe `0x1089a5000`.
A VM region query then found a read/write region at `0x1089a4000`, length
`0x1000`, with VM user tag **230 (`VM_MEMORY_ROSETTA`)**. It was translator
storage, not the retained DIB backing. Instrumenting the release path changed
the reproduction, consistent with the bootstrap-input sensitivity already seen.

The fix replaces the allocating probe with `mach_vm_region`: the next region
must begin at or beyond the requested end, or the query must report no further
region. Other query errors reject availability. Any returned object port is
released. Existing low-reserve bookkeeping and all overlap checks remain; the
bitmap test is unchanged. Availability checks no longer create and tear down a
mapping that can perturb Rosetta's storage placement.

There is an important limit to source-commit attribution: **all bootstrap
sources and their CMake definitions are identical to `macos`** before this fix
(`git diff macos..8ca7b34 -- native/platform/darwin/guest native/CMakeLists.txt
CMakeLists.txt` is empty; the bootstrap stanza in darwin.cmake is unchanged).
The bootstrap links no FEX code. The existing baseline binary reports Mach-O
SDK/minimum OS 27.0, versus 27.2 in this build; compiler cache versions are
21.0.0.21000334 and 21.0.0.21000401 respectively. Thus the binary-dependent
trigger is exposed by the rebuilt bootstrap/toolchain; it cannot honestly be
assigned to one of the listed FEX integration source commits. The observed
Rosetta region identifies the conflicting owner; the precise internal Rosetta
allocation timing is not established.

Validation used RelWithDebInfo, `/tmp/raven-review-modules`, and the provisioned
RTL already configured in build-integration. `cmake --build build-integration
-j8` completed successfully. Every CTest invocation, including investigative
runs, went through `/tmp/raven-ctest-lock.sh`. Final commands:

```sh
RAVEN_PREFIX_SETUP=1 RAVEN_FEX_MODULE_ROOT=/tmp/raven-review-modules \
  /tmp/raven-ctest-lock.sh bitmap-final-20 \
  ctest --test-dir build-integration \
  -R '^native_compatible_bitmap_contract$' --repeat until-fail:20 --output-on-failure

RAVEN_PREFIX_SETUP=1 RAVEN_FEX_MODULE_ROOT=/tmp/raven-review-modules \
  /tmp/raven-ctest-lock.sh bitmap-related-final \
  ctest --test-dir build-integration -j1 \
  -R 'compatible_bitmap|bitblt|dib|memory|fex' --output-on-failure \
  --output-junit /tmp/raven-bitmap-related-final.xml
```

- Unchanged compatible bitmap contract: **20/20 consecutive passes**, 1.26 s.
- Related selection: **206/206 passed, zero failures/skips**, 61.71 s. Includes
  compatible_bitmap, bitblt, dib, memory, and every registered name containing
  fex (including fresh-prefix lanes that unset prefix setup internally).
- `git diff --check` passed. No debugger, push, other-worktree edit, or shared
  component/module edit was used.

The full final repetition log is `/tmp/raven-bitmap-final-20.log`; the related
log/XML are `/tmp/raven-bitmap-related-final.{log,xml}` (local-only). This is a
focused repair qualification, not a new full-suite qualification.
