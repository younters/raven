# mtld3d on FEX: x64 and WoW64

Qualified on 2026-10-31, Apple M4 Pro, macOS 27.2. Both PE widths render the
D3D9 clear/triangle fixture, present 60 frames to a Cocoa window, and pass GPU
readback checks. This supersedes the earlier incomplete bring-up report.
The implementation is opt-in and uses private artifacts under
`/tmp/raven-mtld3d`; no shared component or Rosetta graphics default was changed.
The branch was rebased onto `macos` at `fec1709d` (including the thread-context
and Mullion merges).

## Upstream findings

`git fetch origin` completed again before qualification; `origin/main` remains
`3cd4ca605cb98e56ff4ff6179441d32dba9ed33d`. Its tracked tree has no mtld3d/FEX
integration. GitHub searches of mirkobrombin/raven's open PRs/issues returned no
mtld3d or ARM64EC matches. The open WoW64 PR match was
[PR 99](https://github.com/mirkobrombin/raven/pull/99), concerning UCRT MinGW
installer builds, not this graphics bridge. Earlier broader issue searches
also found no matching integration.

[mtld3d v0.15.1](https://github.com/athei/mtld3d/tree/v0.15.1), pinned to
`9919759fa7c03362deac1b985db6d93de9e33ecf`, already supplies arm64 unix libraries,
ARM64X PE libraries, and i386 PE callers for Wine WoW64. Raven uses the released
x86_64/i386 Wine PE pairs, not an ARM64EC rebuild. Upstream's two 22-entry unix
arrays use identical handlers because its wire structs already have fixed
8-byte alignment and u64 address fields. Raven's biased WoW64 window still
requires address translation.

[CodeWeavers' arm64 preview](https://www.codeweavers.com/blog/mjohnson/2026/7/31/crossover-preview-the-right-to-bear-arm64-on-mac)
describes FEX integration but excludes D3DMetal from that preview. Wine's WoW64
unix-table convention supplies the dispatch model, not Raven's HWND transport
or guest-window translation. The previous `aa920cfb` investigation correctly
identified these integration gaps.

## Design

### Unix dispatch and guest addresses

The loader caches handles by library path and validates the selected table
against private metadata. mtld3d has 22 entries; DXMT retains its 132/146
layouts; the merged Mullion selection and its bounded table validation remain
available. mtld3d never goes through DXMT's ordinal-specific wrappers. Libraries
and adapter state stay loaded for native encoder/GPU callbacks.

PE64 continues through the existing syscall gate. PE32 gets an ntdll **data**
export containing a 32-bit gate address. Its stdcall gate uses `int 2e` and
`ret 16`; the bridge decodes `handle:u64, ordinal:u32, args:u32` from the guest
stack, checks readable stack/header memory, then adds the WoW64 window base to
the argument address. Class 1001 unix-library discovery writes a 64-bit library
handle but a 32-bit `ReturnLength`. The guest's suspend doorbell and EAX/EIP
packet offsets are preserved across calls and scheduler yields.

No native pointer is obtained by reinterpreting an arbitrary PE32 C struct.
The released PE producer already widens each pointer-bearing wire field and
pads its packet to the shared ABI. The native port translates those **guest
memory** fields when dereferenced:

| Boundary | Translation |
|---|---|
| Logger, config, shader tokens/semantics, cursor pixels, arrays | Nonzero 32-bit guest address → window base + offset |
| Encoder packets, metadata, draw records, operation arrays, buffers, counters | Translate nested/retained addresses on the native consumer, including worker threads |
| Completion queue and page/lease nodes | Translate on native reads; subtract the base when publishing a guest-linked node |
| Metal objects, encoder handles, scalar IDs and HWNDs | Preserve full u64 values; never truncate or bias opaque handles |
| PE64 and null addresses | Identity mapping |

`raven_address` unit tests cover nulls, both 32-bit limits, reverse publication,
native addresses above 4 GiB, and x64 identity. Shared compile-time layout
assertions remain in effect. The integration test exercises actual shader
translation, guest vertex data, asynchronous encoder/completion traffic, and
GPU writes into guest-readable memory in both widths.

The x86 runtime now services unix discovery before its normal NtQueryVirtualMemory
handler and pumps the same native event/presentation services as x64. Patch
**0027** repairs an independent startup blocker: WoW64's Raven fault gate now
passes JIT-origin datatype-misalignment faults to FEX's existing unaligned TSO
handler. Native/Mach-O faults remain outside that repair boundary. Its private
build ID is `mtld3d-0027`; x64 xtajit64.dll is unchanged.

### Presentation

The existing Wine-style macdrv table returns DXMT wrapper objects; mtld3d
expects real Cocoa views and a CAMetalLayer. Handing it those wrappers is not
ABI-compatible. The small native source port replaces attachment/retirement
and drawable presentation with explicit Raven entry points.

`presentation.m` reuses DXMT's IOSurface pool, slot state machine, retirement
acknowledgements and command-buffer completion publication by including the
existing implementation. A CAMetalLayer subclass supplies its pooled textures.
The host's existing `DarwinDXMTPresentation` imports those IOSurfaces and sends
them through the established HWND/CAMetalLayer path. Drawable size and pixel
format changes update the pool. mtld3d's classes and macdrv symbol are renamed,
so they cannot replace DXMT's classes/table in the same process. The adapter
checks Raven's shared protocol version. No duplicate Cocoa compositor was added.

Qualified scope is ordinary SDR window rendering. Wine's attachment registry,
HDR/display migration, software cursor overlay, occlusion tracking, and exact
vsync/display-time callbacks are not implemented by this adapter. The port uses
the shader present route, scale 1 and unthrottled fallback; the test explicitly
requests immediate presentation. Shader-cache path translation is also absent.
These limitations are reported by mtld3d and are not general game-compatibility
or frame-pacing claims.

### Runtime prerequisites

The released PE DLLs use `vcruntime140.dll`; no msvcp140 dependency was needed
for these fixtures. Private application-local copies satisfy normal Raven DLL
loading. The x64 copy came from an existing local Steam redist; the x86 copy was
extracted from Microsoft's `https://aka.ms/vc14/vc_redist.x86.exe`. Both are
hashed in the evidence manifest. Raven's existing `redist_fixtures` and
`redist32_fixtures` were built, and `x86_ucrt_component` supplies the required
32-bit API-set/UCRT implementation. No system installation was performed.

Startup additionally needed `VirtualAlloc2FromApp` aligned unconstrained
reservations, width-correct `GetCursorInfo`, and x86 routing of the Rtl vectored
handler aliases to Raven's existing UCRT implementation. Non-default address
bounds/unsupported extended allocation parameters fail explicitly. Focused
PE32/PE64 tests cover these allocation and cursor contracts.

snmalloc reserves 256 GiB on x64. FEX now represents reserved PROT_NONE pages
implicitly by region, storing explicit page protections only where needed.
The regression test reserves that range, commits/decommits a 16 KiB interval,
checks 4 KiB neighbour preservation, then releases it without per-reserved-page
allocation or traversal.

## Rendering evidence

`run_fixture.py` creates a fresh prefix and refuses stale run directories.
It requires exit zero and `window.txt == OK`, then checks the first and last
256×256 RGBA GPU readbacks. Triangle-edge pixels are excluded; interiors and
backgrounds are checked exactly, including alpha. Asynchronous shader
compilation is disabled to make the first-frame oracle deterministic.

| FEX fixture | Frames presented | Checked pixels per readback | Triangle samples | Mismatches, first / last |
|---|---:|---:|---:|---:|
| x64 | 60 | 63,042 | 20,402 | 0 / 0 |
| WoW64 x86 | 60 | 63,042 | 20,402 | 0 / 0 |

Both widths have byte-identical readbacks:

- first: `8b625261408d722c12c93765fb288199c89ccfca4d8b3e10641beea61ee918d0`
- last: `60c308dc165b893e8d374f8df9fccbd89bc65538a1a654d42c29fe1e8c212f9d`

Final logs, raw data, PNGs, screencaptures and binary hashes are in
`/tmp/raven-mtld3d/final32/` and `/tmp/raven-mtld3d/final64/`.
The screen was unlocked; both screencaptures show the foreground red triangle.
Readbacks establish GPU correctness independently of the screen state.
[qualification.json](qualification.json) records the compact evidence and hashes.

The independently downloaded, unmodified Microsoft
[Direct3D Tutorial 02: Vertices](https://github.com/microsoft/DirectX-SDK-Samples/tree/07e3eaa10e7dd026ec9d95fe326db2d5c4227e1b/C%2B%2B/Direct3D/Tutorials/Tut02_Vertices)
(MIT) was built as PE32 with MinGW's static C++/GCC runtime. It creates a device,
uses a vertex buffer, and presents its interpolated colored triangle in a Cocoa
window. `/tmp/raven-mtld3d/demo/screen.png` records it. The original sample has an
infinite message/render loop, so the run ends at the configured 15-second
Raven timeout; no successful application exit is claimed. This is an external
sample, not qualification of a full game.

### Rosetta

A separate run using the stock **x86_64** mtld3d.so under `--execution=native`
also passed the same two offscreen readbacks. Window attachment explicitly
failed with `this Wine publishes no macdrv_functions table`. Thus offscreen
rendering is usable there, but window presentation is **not** integrated.
Present returning success alone is insufficient evidence. The arm64 port is
FEX-only, and Rosetta's agent, graphics transport and default selection are
unchanged. Logs/readbacks: `/tmp/raven-mtld3d/rosetta/`.

## Reproduce privately

Use RelWithDebInfo and copy FONT_HASHES, PNG_LIBRARY_RELEASE,
PNG_PNG_INCLUDE_DIR, all RAVEN_*_COMPONENT*, RAVEN_DXMT_* and RAVEN_SQLITE_*
settings from the established build cache. The private configured build is
`/tmp/raven-mtld3d/build`; `configure-args.txt` records the initial copied paths.
Configure FEX on, and select the private module root after building patch 0027:

```sh
python3 scripts/build-fex-modules.py --root /tmp/raven-mtld3d/fex-private \
  --raven-host --build-only --build-id mtld3d-0027
python3 native/platform/darwin/mtld3d/build_arm64.py \
  --source /tmp/raven-mtld3d/upstream-clean \
  --destination /tmp/raven-mtld3d/new-overlay
cmake --build /tmp/raven-mtld3d/build --target raven x86_ucrt_component \
  darwin_mtld3d32_fixture darwin_mtld3d64_fixture \
  darwin_mtld3d_runtime32_fixture darwin_mtld3d_runtime64_fixture \
  redist_fixtures redist32_fixtures -j8
```

The native builder requires a clean source checkout at the pin, copies its
tracked inputs, applies `patches/0001-raven-iosurface-wow64.patch`, and builds
Rust release with debug information plus the arm64 adapter. A complete clean
provisioning run passed in `/tmp/raven-mtld3d/provisioned`; its manifest captures
commands, source/patch hashes, compiler and native artifact hashes. It preserves
a disk reserve and never writes back to the input checkout.

Stage the release's `wine/i386-windows/{d3d9,mtld3d}.dll` and x86 vcruntime into
`ROOT/pe32`, and the corresponding `x86_64-windows` pair/runtime into `ROOT/pe64`.
The release archive SHA-256 is
`115d9089323b20010738d2b4835e79e6652d41690c4b360141e8b0433faff972`.
The application-local DLLs opt in; no system D3D9 default changes.

```sh
RAVEN_MTLD3D_ARM64_ROOT=/tmp/raven-mtld3d /tmp/raven-ctest-lock.sh mtld3d \
  ctest --test-dir /tmp/raven-mtld3d/build --output-on-failure \
  -R '^darwin_mtld3d(32|64)_fex$'
python3 native/platform/darwin/mtld3d/run_fixture.py \
  --raven /tmp/raven-mtld3d/build/native/raven \
  --fixture /tmp/raven-mtld3d/build/native/raven-mtld3d32.exe --width 32 \
  --modules /tmp/raven-mtld3d/fex-private --artifacts /tmp/raven-mtld3d \
  --output /tmp/raven-mtld3d/new-evidence32 --capture
```

Use a new output directory and substitute width/fixture 64 for x64. Rendering
ctests skip only when the private artifact root is not selected. Every ctest
invocation in qualification used the shared lock wrapper. No debugger was
attached, no shared components were modified, and nothing was pushed.

## Regression results

All fixture targets were built explicitly with `-j8`, including the existing
D3D9/redist targets, all DXMT fixtures, the native pool/slot tests, and the
window-info helper. Final D3D9 ctests passed in both widths (22.55 seconds for
the pair and prefix cleanup). The native clean provisioner, Rust address tests,
PE32/PE64 runtime contracts, sparse reservation contract, WoW64 unaligned-store
contract and existing WoW64 access-violation recovery check all passed.

DXMT results across the locked batch and focused rechecks:

| Engine | Completed passing DXMT checks | Remaining check |
|---|---:|---|
| Rosetta (`native`) | 21 / 21 | none |
| FEX | 20 / 21 | `features` rejected before graphics: fixed PE preferred base unavailable, no relocation table |

The initial batch had four failures: two missing `dxmt-window-info` helper
executions and two pre-graphics placement collisions (`msaa`, `features`).
After explicitly building the helper, both window tests passed, including
presentation/resize verification. `msaa` passed its focused recheck;
`features` again hit the same placement rejection. This is the already tracked
fixed-base placement issue under concurrent work, not a successful graphics
test and not suppressed here. No shader/pixel regression was observed in the
checks that entered graphics. Native DXMT pool and slot-state tests also passed.
The full first batch was 91/95 including cleanup; the evidence manifest keeps
all attempts, not only passing rechecks.

Logs: `/tmp/raven-mtld3d/dxmt-regressions.log`, `dxmt-window-recheck.log`,
`dxmt-placement-recheck.log`, and `final-fixtures.log`.

## Private artifact SHA-256

| Artifact | SHA-256 |
|---|---|
| `mtld3d.so` (arm64, clean provision) | `dda5d5a9b36e83e8f56087ee292fb02c98130e3f44b2f94c5341a2d8768028b2` |
| `raven-mtld3d.dylib` (arm64) | `9e9a2a1e73a6bab934157004b36450a95e738b3ee5401b351eddd4fc72bf90c2` |
| `fex-private/xtajit.dll` (patch 0027) | `a4dcb4a01c3ea03b360a71c8412c31fafb404bd5ae83142844e6a9f067695e70` |
| `fex-private/xtajit64.dll` (unchanged) | `73d4d781cd705ace3c6c298c9b3d19dece1f0a9aa2e1960dbef9bf8646b5c2a2` |

PE DLL, fixture, runtime, source patch and screenshot hashes are in
[qualification.json](qualification.json); native rebuild provenance is also
`/tmp/raven-mtld3d/provisioned/artifacts.json`. Disk free remained above 8 GiB
(about 10 GiB at final verification).
