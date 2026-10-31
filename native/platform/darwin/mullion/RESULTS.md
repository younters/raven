# Mullion qualification attempt, 2026-10-31

Base: macos `10f7e3818c247925c0b901125caa69da0dd626a1`.
Branch: `impl/mullion-d3d12`. Experimental, not a default backend.

## Source evaluation

Mullion source: https://github.com/dttdrv/mullion, pinned to
`46e6f0508c4d48dd1aec3ac22df4da87cad102a8`, cloned recursively into
`/tmp/raven-mullion`. License: **LGPL-2.1-or-later**; keep the DLLs and
unix libraries separate. The source checkout, patch and license remain with
private build artifacts. Exact submodule pins and artifact SHA-256 hashes are
in [artifacts.json](artifacts.json).

Upstream's README claims D3D12 feature levels through 12_2, SM 6.7, RT tier
1.1, mesh/amplification shaders, VRS tier 2, conservative rasterization tier 3,
tiled resources tier 3, sampler feedback, pipeline libraries and D3D11On12.
FL 12_0+ needs macOS 26.4 placement sparse resources. Its documented gaps
include advanced SM 6.7 texture operations, writable MSAA textures and stream
output after tessellation. These are upstream claims, not Raven qualification.
DXGI uses a private IMTLSwapChainFactory interface on the device/queue;
D3D12 device creation queries the private IMTLDXGIAdapter interface.

Mullion builds x64 PE with llvm-mingw, and Mach-O unix code with Apple clang,
Metal tools and static LLVM 15. It also supports ARM64EC builds, but this
attempt uses **x64 PE for both Raven engines**. ARM64EC shipping support is
not proof that Raven can load those binaries. The build normally chooses the
unix architecture from the PE architecture; our small Meson patch allows an
arm64 unix side with x64 PE for FEX. Rosetta uses x86_64 unix code.

The table has 197 entries versus DXMT's 132/146. The first 146 entry names
match, but argument ABI is not entirely unchanged: CreateMetalViewFromHWND
inserts a 40-byte placement record. Raven translates that call to its IOSurface
view wrapper. Event notification adds handle-retention semantics; Raven's
existing callback wrapper does not yet implement the new retention guarantee.
Mullion also adds Metal 4 queue/presentation, sparse mapping, timestamp,
acceleration-structure and diagnostics calls. Existing wrapping does **not**
qualify these new APIs. Qualified fixtures explicitly use
`RAVEN_GUEST_MULLION_D3D12_METAL4=0`.

Do not mix a lone Mullion d3d12.dll with DXMT winemetal/DXGI. The opt-in uses
Mullion's matched graphics DLL family for the selected process. DXMT remains
the default for unselected applications. Same-process mixed versions are not
supported or tested.

`git fetch origin` completed. Searches of mirkobrombin/raven PRs and issues
found no Mullion work. [PR #59](https://github.com/mirkobrombin/raven/pull/59)
is merged D3D12 DLL discovery work, not a Metal backend.
[Issue #25](https://github.com/mirkobrombin/raven/issues/25) is broader
application compatibility discussion. Search snapshots are in the private root.

## Build and selection

All artifacts are private: `/tmp/raven-mullion-components`, including PE DLLs,
`unix64`, `unix-arm64`, Wine import-library build, Meson builds and logs.
Shared raven-components was only read. Native adapters link to the separately
built winemetal.so; no Mullion implementation is linked into Raven.

Raven: `build-mullion`, RelWithDebInfo, Ninja `-j8`, FEX modhost enabled with
`/Users/yount/Documents/Projects/raven-components/fex-modules`. FONT_HASHES,
PNG_LIBRARY_RELEASE, PNG_PNG_INCLUDE_DIR and all RAVEN_*_COMPONENT*,
RAVEN_DXMT_* and RAVEN_SQLITE_* cache values were copied from build-macos.
Skia/font paths were also carried over. All four `mullion_*_fixture` targets
were explicitly built. The fixtures reuse existing testdata/d3d12 sources:
device feature levels, clear/copy/fence/readback, root-signature/PSO triangle,
and a queue/swapchain presenting and resizing with readback.

Build prerequisites resolved during this attempt: Wine's old build directory
had lost its import libraries, so these were rebuilt privately from Wine 10.0
source; Homebrew bison was required. Xcode lacked its Metal toolchain;
`xcodebuild -downloadComponent MetalToolchain` installed 27B47. No FEX patch
was needed. `unreachable.c` supplies fail-fast symbols for native Wine handle
operations referenced only by table entries replaced by Raven's adapter.

For a fresh private build, put llvm-mingw on PATH and run `build.py --help`.
Supply a clean checkout at the pin, a NEW destination, the configured private
Wine build, and the x86_64/arm64 static LLVM 15 roots. It applies the recorded
Meson patch and builds with eight jobs. It checks free space before each
architecture; monitor free space during compilation as well.

Runtime selection (or configure RAVEN_MULLION_ROOT through CMake):

```sh
RAVEN_D3D12_BACKEND=mullion \
RAVEN_MULLION_ROOT=/tmp/raven-mullion-components \
RAVEN_GUEST_MULLION_D3D12_METAL4=0 \
build-mullion/native/raven --run-process --execution=fex --timeout-ms=80000 \
  --desktop=macos build-mullion/native/mullion-device.exe
```

Replace `fex` with `native` for Rosetta. No changes to the shared component
installation are required. Unset RAVEN_D3D12_BACKEND to return to DXMT.

The supplied shared CRT was missing thirteen C-locale exports used by libc++.
A separate Raven CRT commit adds them; a private build is in
`controls/ucrtbase.dll` and was copied into `build-mullion/native/ucrtbase.dll`
for qualification. The normal cache still names the supplied shared controls
root, so rebuilding Raven can overwrite this private CRT copy. Recopy it before
repeating these tests, or configure a complete private controls overlay.
`crt_locale.c` checks successful conversion, ERANGE, EILSEQ and C-locale
classification/collation; it exited zero under both engines.

During the work the shared FEX module changed and stopped matching this base
branch's hardcoded RavenProcessInit RVA. The private qualification copies
`/tmp/fex-modules-backup-0019` into `fex-modules` under the Mullion root and
selects it with RAVEN_FEX_MODULE_ROOT. This matches RVA 0x6cbc and the host ABI
v2 checks. No shared module was changed, and no contract check was weakened.

## Qualification and remaining gaps

The first complete Metal 3 run passed **8/8 graphics cases** (CTest reports
16/16 including eight prefix-cleanup jobs): device, clear/fence, draw and
window12 under native/Rosetta and FEX. `final.xml` and `final.log` in the
private root preserve results. `window12` presents sixty frames, reads back
the triangle, resizes, and exits successfully; the harness checks both
readbacks against Raven's independent triangle rasterizer. The resized
320x200 image passed on both engines with worst channel error 1. Clear checks all
4096 RGBA pixels exactly. Device reports `hr=00000000 level=0000c200 query=ok`
under both engines. These tests exercise fence signaling and event completion
as well as a root signature and graphics PSO.

Both `build-mullion/native/mullion-window12-{native,fex}/screen.png` captures
were inspected: each shows the colored triangle in a native macOS window.
The display was **not locked**. Captures include the surrounding desktop and
are local-only; do not publish them as repository artifacts. Offscreen pixel
checks are present independently of screen visibility.

Initial failures are preserved rather than counted as passes: missing CRT
imports, shared FEX contract drift, changed view ABI, missing drawable
presentation-statistics callback, and unintended Metal 4 selection. Host
`MULLION_D3D12_METAL4=0` did not reach Windows; use the RAVEN_GUEST_ prefix.
Raven now defaults selected Mullion processes to Metal 3 in the Windows
environment; an explicit guest variable can override it for research. Early
non-window tests passed with Metal 4 as well, but its native drawable queue
methods are **not supported for presentation**.

The optional presentation-count callback is disabled: it observes no frames,
leaving upstream's statistics storage unreported. No display timestamp is
fabricated from GPU completion. The upstream PE thunk asserts on nonzero
unix status, so omitting observations must return zero for that void
bookkeeping call. Accurate DXGI present statistics remain a gap.

Other unqualified areas: closing/reusing an event handle before its GPU
notification completes (new Mullion handle-retention semantics), ARM64EC PE,
32-bit apps, same-process DXMT/Mullion mixing, HDR, multiple windows/adapters,
advanced FL12_2/SM6.7 features, RT, mesh shaders and long-running games. The
advertised feature level is not evidence that all those paths work in Raven.

The first warm complete run took 59 seconds including application sleeps;
window fixture durations were roughly 27–28 seconds. Clear and draw tests
completed in under a second after startup. These are qualification timings,
**not FPS benchmarks**, and cannot establish game performance. Initial cold
Rosetta launches sometimes took about ten seconds.

Recommendation: retain this as an **experimental opt-in backend**, with private
artifacts reviewed before any shared provisioning. It is substantially beyond
watching upstream: both execution engines render and pass pixel/fence tests.
Keep DXMT as the D3D10/11 default; do not promote Mullion to a general graphics
default until the event lifetime and presentation-statistics gaps, Metal 4
presentation, application coverage and performance have been qualified.

All CTest invocations went through `/tmp/raven-ctest-lock.sh`; no debugger was
attached, no push was made, and no shared component, other worktree or macos
branch was modified. Observed free disk remained above 8 GiB (lowest observed
about 9.6 GiB).


The rebuilt final runtime repeated all eight graphics passes (`final-code.xml`;
67.52 seconds including cold startup). The two additional command-line CRT
accessors were then checked in the focused CRT probe on both engines. Default
DXMT regression checks passed **4/4**: device and triangle on each engine
(`dxmt-regression.xml`). [results.json](results.json) records individual results;
CTest cleanup jobs are excluded from those graphics counts.

Reproduce the graphics run with the private compatible module and CRT:

```sh
cp /tmp/raven-mullion-components/controls/ucrtbase.dll build-mullion/native/ucrtbase.dll
RAVEN_PREFIX_SETUP=1 \
RAVEN_FEX_MODULE_ROOT=/tmp/raven-mullion-components/fex-modules \
/tmp/raven-ctest-lock.sh mullion ctest --test-dir build-mullion -j1 \
  -R '^darwin_mullion_' --output-on-failure
```

## Microsoft HelloTriangle attempt

Built the MIT-licensed Microsoft DirectX-Graphics-Samples HelloTriangle at
`68a2b1e97f71642924e74c7757ca21756ad221b5` with llvm-mingw, using separately
pinned DirectX-Headers (see manifest). The private source/headers, licenses,
executable, shaders and compatibility header are retained under the component
root. MinGW adjustments supply WRL's small FileHandle helper and missing UUID
specializations, spell `_uuidof` as `__uuidof`, disable unused state-object
helpers, and adapt the two descriptor-handle getters to explicit output
parameters. An extra diagnostic prints a failing HRESULT before throwing;
this did not produce an HRESULT in the observed failure.

**The sample did not render.** Both engines pass initial startup/device and
swapchain setup far enough to log MakeWindowAssociation, then exit with status
3 before a capture. `sample-{native,fex}.log` and
`sample-diagnostic-results.log` retain this outcome. The remaining failure
is not diagnosed; do not treat the fixture results as application qualification.
The first attempt additionally exposed `__p__acmdln`; a separate CRT commit
adds command-line accessors and the focused CRT test verifies the narrow one.

The sample's copied `d3dcompiler_47.dll` is the **Microsoft Windows SDK
redistributable**, not Wine/LGPL code. Its original provenance JSON, exact
Microsoft download URL and archive hash are retained beside it. This dependency
is sample-only: all graphics fixtures use precompiled shader bytecode. No
proprietary compiler DLL is committed to Raven. Wine's retained license applies
to the Wine import-library build, not this Microsoft binary.
