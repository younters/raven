# FEX DXMT qualification, 2026-10-31

Implementation branch: `impl/fex-dxmt-arm64`, based on `e4d388d7`.
The runtime/test implementation ends at `d01c2a729f38f124ff365e2f4939b1816a45c706`;
the following documentation commit records its qualification.

## Design and changes

See [GRAPHICS.md](../GRAPHICS.md#fex-same-process-dxmt-design-2026-10-31).
The x64 PE dispatcher uses a reserved syscall gate and the existing FEX frontend
native callback boundary, preserving Darwin x18 and the captured guest context.
The host validates opaque library handles/ordinals and calls the retained arm64
unix table. No FEX module patch or module rebuild was required.

The private arm64 adapter exports DXMT's macdrv_functions table. HWND/view
wrappers retain the existing IOSurface drawable and host presentation path;
Mach send rights are retained locally. Native event completions are drained by
the controller. Live Metal buffer ranges have a synchronized, bounded readback
bridge for host APIs such as WriteFile. Arbitrary host memory is not admitted
into the guest page map.

Three runtime defects exposed by this work have separate fixes:

- `62b12ac4`: remove present → place_host_surface → place_hosted → present
  recursion. The abort backtrace established stack exhaustion in this shared
  window path. This also repairs the five baseline Rosetta DXMT failures.
  It is a separate prerequisite commit, useful if another branch already fixes
  the same recursion. Rosetta's unixlib, assembly gate and protocol-v5 transport
  are unchanged.
- `80a21672`: FEX must not use the host/Rosetta RDTSC calibration for QPC;
  it now keeps the host API fallback. The pacing fixture verifies agreement
  between QPC and tick time.
- `d01c2a72`: account for cumulative cooperative time slices across short API
  calls, honor pending interrupts after unix calls, and service native graphics
  work periodically. The features fixture's tight GetData/GetTickCount loop
  previously starved its completion worker and failed after 60 seconds. It now
  passes without changing the fixture or its query/pixel assertions.

The new engine path is opt-in through `RAVEN_DXMT_ARM64_ROOT`; PE modules still
come from the existing RAVEN_DXMT_ROOT. The unix-call-count metadata must match.
This implements x64 DXMT, not a new WoW64 DXMT unix table. The existing x32 keyed
helper fixture is not evidence of 32-bit D3D rendering.

## Build and private artifacts

RelWithDebInfo, Ninja, `-j8`, RAVEN_FEX_MODHOST=ON and the supplied
`/Users/yount/Documents/Projects/raven-components/fex-modules` were used.
FONT_HASHES, PNG_LIBRARY_RELEASE, PNG_PNG_INCLUDE_DIR and all
RAVEN_*_COMPONENT* cache entries were copied from build-macos. The baseline's
DXMT root, Skia source/library, font directory and certificate root were also
selected. Without the Skia/font settings, CreateFontW was unavailable; that
initial Steam attempt is not the final application qualification.

The build is `build-fex-dxmt`. DXMT fixture targets and window-info were built
explicitly. No full suite was run. All CTest invocations used
`/tmp/raven-ctest-lock.sh`, including inventory, baseline and rechecks.
The baseline tests used existing executables and a private CTest manifest under
`/tmp/raven-dxmt-arm64/baseline`, with writable output outside the baseline tree.
The baseline manifest selected AMD identity; normal final CTest properties were
retained (identity tests select AMD).

The source overlay pin is `3b252590cb0290e36fca6f088b5255b871d2d95a`, 146 entries.
The native build reuses generated headers/compile commands from the provisioned
source build and arm64 Homebrew LLVM 15; it does not modify that build.
Reproduction:

```sh
python3 native/platform/darwin/dxmt/build_arm64.py \
  --source-build /Users/yount/Documents/Projects/raven-components/dxmt-source-build \
  --destination /tmp/raven-dxmt-arm64-new
```

The destination must be new. The helper copies the source tree, verifies the
pinned table prefix, builds only native objects with eight jobs, builds Raven's
adapter, fixes the loader-relative dependency, signs the library and writes an
artifact hash manifest. `/tmp/raven-dxmt-arm64-repro` reproduced the original
winemetal and adapter hashes. Both libraries were verified as arm64 Mach-O.

Current artifacts, in `/tmp/raven-dxmt-arm64`:

| File | SHA-256 |
| --- | --- |
| winemetal.so | e264623dfa46b5dacd53dde48a03980dcf1ded818beb0110a0a5b7497a48cd07 |
| raven-dxmt.dylib | 3e9ffbad292b056036e2c4b82b82ef4c6457d0f2a67d5a757d1c7bcb10f72342 |
| unix-call-count | 4c005f84cfaf4ccea894c12b193c1fba0207ffd615da6f61f1f8c130d2a0c9f1 |

The private root also retains sources, generated headers, build logs, manifests
and test evidence. No shared component or other worktree was modified, no push
was made, and no debugger was attached. Disk checks remained above 8 GiB
(minimum observed about 13 GiB). Only an owned features process was explicitly
aborted for a backtrace; temporary native-fault abort instrumentation was removed.

## Tests

The committed [results](dxmt-results.json) preserve individual outcomes and
failure excerpts. Full logs/XML under `/tmp/raven-dxmt-arm64` are local-only.

- Baseline: **16/21 passed**. device10, pacing, window, fullscreen and present_rate
  failed, matching the known baseline list.
- Final focused run: **46/50 passed**: 21 Rosetta DXMT, 21 FEX DXMT, and eight FEX
  TLS/JIT tests across widths and scheduling modes were selected. All 21 Rosetta
  tests passed. Four tests failed before guest execution with `PE preferred base
  is unavailable and image has no relocation table`: FEX pool, triangle, compute,
  and native_jit_stress_x64_fex_1.
- The single serial recheck of those four passed **4/4**. Thus every selected
  fixture has a passing final-code run, but the 50-test invocation was **not**
  uniformly green. The pre-existing address-placement intermittency remains.
- The corrected cooperative features/pool/pacing selection passed **3/3**;
  features also passed with RAVEN_PARALLEL=1. Earlier failed query runs and
  investigative aborts preceded the cumulative-slice fix.

Final commands (after a disk check):

```sh
RAVEN_PREFIX_SETUP=1 RAVEN_DXMT_ARM64_ROOT=/tmp/raven-dxmt-arm64 \
  /tmp/raven-ctest-lock.sh dxmt-complete ctest --test-dir build-fex-dxmt -j1 \
  -R '^darwin_dxmt_|^native_(tls_slots|jit_stress)_x(32|64)_fex_' \
  --output-on-failure --output-junit /tmp/raven-dxmt-arm64/complete.xml

RAVEN_PREFIX_SETUP=1 RAVEN_DXMT_ARM64_ROOT=/tmp/raven-dxmt-arm64 \
  /tmp/raven-ctest-lock.sh dxmt-complete-recheck ctest --test-dir build-fex-dxmt -j1 \
  -R '^darwin_dxmt_(pool|triangle|compute)_fex$|^native_jit_stress_x64_fex_1$' \
  --output-on-failure --output-junit /tmp/raven-dxmt-arm64/complete-recheck.xml
```

Pixel checks, query results, compute values, sharing, presentation, resize,
fullscreen and pacing remain strict. A successful device creation alone was
not counted as successful rendering. The validated FEX window capture is
`/tmp/raven-dxmt-arm64/fex-window.png`.

## Steam

The old prefix was APFS-cloned into `/tmp/raven-fex-dxmt-steam`. Runs unset
RAVEN_PREFIX_SETUP, use the private arm64 overlay and existing PE overlay, and
launch Steam with `--run-process --execution=fex --desktop=macos`, a 70-second
runtime deadline and its installation directory. Both cooperative and parallel
scheduling were tried; the final run used cooperative scheduling after the fix.

Steam completed updater verification and launched CEF. Its fresh GPU report
identifies ANGLE_D3D11 and `ANGLE (Apple, Apple M4 Pro ... Direct3D11 vs_5_0 ps_5_0)`;
Raven's log records DXMT feature level 11_1. The missing arm64 unixlib/device
creation blocker is removed.

**Login UI was not verified.** CEF still reports NOTREACHED checks, libcef
breakpoint exceptions, pipe failures and VirtualQuery failures. These were not
fixed or hidden with a software-rendering or sandbox-disable switch. Media
Foundation is also absent, as recorded by the GPU report. Root-cause attribution
of the remaining CEF checks is not established here.

`/tmp/raven-dxmt-arm64/steam-{20,40,60}.png` were taken with `screencapture -x`.
Inspected captures show macOS permission dialogs (local-network/microphone), not
an established Steam login window. No microphone permission was granted.
Earlier captures/logs are retained in steam-cooperative/ and steam-parallel/.
The final log is steam.log; bootstrap_log.txt and webhelper_gpu.txt preserve
application-side updater and Direct3D evidence. These screenshots are local-only
and intentionally not committed because they include the surrounding desktop.
