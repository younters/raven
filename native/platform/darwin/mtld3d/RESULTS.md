# mtld3d FEX bring-up, 2026-10-31

**Incomplete: no working D3D9 rendering backend is claimed.** This branch adds
private arm64 provisioning, opt-in x64 unix discovery and backend-neutral PE
fixtures. Cocoa presentation and WoW64 address translation remain unimplemented.
Do not promote these artifacts to shared components as a qualified backend.

## Upstream check

`git fetch origin` completed; origin/main was `3cd4ca60`. Its tracked tree has
no mtld3d/FEX/ARM64 implementation. GitHub issue searches in mirkobrombin/raven
returned zero mtld3d and ARM64EC matches; the 24 WoW64 matches concerned general
runtime work, with no matching graphics integration identified.

[mtld3d v0.15.1](https://github.com/athei/mtld3d/tree/v0.15.1), commit
`9919759fa7c03362deac1b985db6d93de9e33ecf`, already supports arm64 unix libraries,
ARM64X PE libraries and i386 PE callers under Wine WoW64. Raven can use the x64
PE pair without rebuilding it as ARM64EC. The release archive checksum matched
the previous investigation at `aa920cfb`.

[CodeWeavers' arm64 announcement](https://www.codeweavers.com/blog/mjohnson/2026/7/31/crossover-preview-the-right-to-bear-arm64-on-mac)
describes FEX integration but explicitly excludes D3DMetal from that preview.
Wine's WoW64 unix-table convention is relevant; neither it nor an arm64 binary
supplies Raven's missing Cocoa objects or biased-address conversion.

## Implemented boundary

The FEX loader distinguishes the requested unix-library basename and count;
mtld3d is pinned to 22 entries, DXMT retains its 132/146-entry validation and
adapter wrapping. Handles are cached by requested path, rather than treating
all loads as handle 1. Unknown library/count pairs and mismatched metadata are
rejected. Native libraries remain retained for asynchronous callbacks.

An application-local `d3d9.dll` and `mtld3d.dll` may opt into x64 discovery using
`RAVEN_MTLD3D_ARM64_ROOT=/tmp/raven-mtld3d`. No default DLL routing or Rosetta
adapter changes were made. The native mtld3d table is deliberately not passed
to DXMT's ordinal-specific wrappers. This is a loader experiment, not usable
window presentation.

## Presentation design still needed

The existing DXMT `macdrv_functions` returns a record whose client pointer is
an HWND and whose metal-view pointer is a RavenDXMTLayer wrapper. mtld3d
messages these as real NSViews and inspects their window/screen hierarchy,
retains/reparents them, installs observers, and uses an actual CAMetalLayer.
Same-process execution solves pointer reachability, but does not make those
wrapper objects interchangeable. Publishing the existing table to mtld3d
would be unsafe.

The next implementation must expose retained Cocoa client views associated
with Raven HWNDs, schedule attachment and removal on AppKit's main thread,
and preserve host surface lifetime/resize/foreign-window routing. Alternatively
port mtld3d attachment and presentation to Raven's IOSurface transport. Do not
silently replace the process-global table while DXMT callbacks may retain it.
A shared superset table must preserve DXMT behavior or the libraries need
explicitly separated resolver hooks.

## WoW64 design still needed

Upstream's native and wow64 arrays contain identical 22 handlers. Its structs
use 8-byte-aligned u64 pointer/handle wire fields even in PE32. Width conversion
is therefore already done by the PE producer. **Address conversion is not.**
Raven's WoW64 guest uses a biased memory window (`State::host`), unlike Wine's
identity-address convention.

A correct implementation must:

1. Supply a PE32 dispatcher gate and decode its stack arguments, including the
   full 64-bit unix-library handle. Discover class 1001 and write the ntdll
   dispatcher using the guest pointer width; the current constructor writes 8
   bytes and must not be reused unchanged for PE32.
2. Validate and translate the parameter block plus each guest-address field.
   Preserve opaque Metal handles and native record handles at their full width;
   do not apply the guest bias to them. Sign/zero extension of scalar values
   must follow each pinned field's type.
3. Translate nested encoder-wire address ranges and persistent pointers, not
   just the outer structs: logger strings, resource backing storage, readback
   destinations, commands, completion mailboxes, and attachment atomic sinks.
4. Preserve asynchronous storage lifetime and prevent queued GPU/encoder work
   from using guest buffers after release. Native-returned addresses exposed
   to PE32 require guest-visible mappings or opaque indirection; truncation is
   invalid. Test high host addresses, nulls, range overflow, retained buffers,
   destruction and simultaneous x64/x86 use before rendering qualification.

The existing WoW64 rejection remains. No FEX patch was added; reserved patch
numbers 0027–0030 were not consumed. Merely calling the upstream wow64 table
would dereference incorrect addresses in this runtime.

## Build and native probe

Built mtld3d with Rust 1.99, `cargo build --release --locked -j8`, from the
pinned source's unix workspace. Its release profile includes debug information.
No Wine SDK is needed for the unix-only build. Native artifact is arm64 Mach-O.
The helper `build_arm64.py` reproduces those steps using a clean copy of tracked
source inputs and writes hashes; it was syntax-checked, not independently
reproduced end to end in this run.

```
python3 native/platform/darwin/mtld3d/build_arm64.py \
  --source /tmp/raven-mtld3d/src --destination /tmp/raven-mtld3d-new
clang -arch arm64 -O2 -Wall -Wextra \
  native/platform/darwin/mtld3d/compatibility_probe.m \
  -framework Foundation -framework Metal -o /tmp/raven-mtld3d/compatibility-probe
/tmp/raven-mtld3d/compatibility-probe /tmp/raven-mtld3d/mtld3d.so
```

Actual native probe output (exit 0 means the expected attachment failure):

```
native/wow64: 22 non-null entries, identical handlers
GetDeviceInfo: status=00000000 device=Apple M4 Pro registry=4294968444 caps=0000009f
AttachMetalLayer without macdrv_functions: status=c0000001 view=0 layer=0
```

This is not a PE fixture, GPU readback, or screen-presentation result. No claim
that a locked screen prevented verification is made. Rosetta remains unusable
for mtld3d presentation through this path: its separate x86_64 process cannot
message the arm64 host's Cocoa objects, as the earlier investigation explains.

Raven configuration is `/tmp/raven-mtld3d/build`, RelWithDebInfo, FEX modhost
on, using the requested shared FEX module root read-only. Requested component,
DXMT, SQLite, PNG and font cache entries were copied from build-macos; exact
arguments are in `/tmp/raven-mtld3d/configure-args.txt`. Builds use `-j8`.

## Private artifacts

Full hashes, including both PE widths, are in `/tmp/raven-mtld3d/artifacts.json`.

| Artifact | SHA-256 |
| --- | --- |
| mtld3d.so, locally built arm64 | 48a49d9e97b8da7cf8fc94882dd0456e772c964319ba8bc449fe7a19eea96ab5 |
| unix-call-count | f14b4987904bcb5814e4459a057ed4d20f58a633152288a761214dcd28780b56 |
| upstream mtld3d.tar.xz | 115d9089323b20010738d2b4835e79e6652d41690c4b360141e8b0433faff972 |

No debugger, shared-component writes, or push. Disk checks remained above
8 GiB (lowest observed so far 11 GiB). No third-party demo was attempted before
the fixture and presentation boundary were qualified.

## Runtime qualification results

Raven and `x86_d3d9_fixture` built successfully. Explicitly built
`darwin_mtld3d32_fixture`, `darwin_mtld3d64_fixture`,
`darwin_dxmt_triangle_fixture`, and `darwin_dxmt_compute_fixture` with `-j8`.
The D3D9 workload now registers a Unicode window class, matching the working
Darwin fixtures, and reports registration/window errors. Its clear, triangle,
present and readback assertions are unchanged. The original ANSI fixture
failed window creation before DLL loading on this host.

The supplied shared FEX root changed relative to this checkout's pin and
initially failed `CHPE RavenProcessInit target/RVA does not match pinned contract`.
Archived `FEX-darwin/build/local/m1d-0019/dlls` binaries matched **both exact
SHA-256 pins in scripts/fex-modhost-pins.json**. Copied only those two binaries
to `/tmp/raven-mtld3d/fex-modules` and selected them with RAVEN_FEX_MODULE_ROOT
for the final runs. No contract check was disabled and no shared file changed.

Final results:

- DXMT Rosetta triangle and compute: **2/2 pass**, including pixel/value checks.
- DXMT FEX triangle and compute with private matching modules: **2/2 pass**.
- D3D9 x64: guest ran, then `FAIL d3d9.dll 0000007e` (module not found).
  Supplying the PE DLL pair explicitly diagnoses `missing PE module: vcruntime140.dll`.
- D3D9 x86: guest ran, then `FAIL d3d9.dll 0000007e`. No WoW64 unix call reached.
- One x64 retry was needed for the known non-relocatable-image address collision.
- No successful GPU readback or screenshot exists. Neither clear, triangle nor
  present is established; no real D3D9 application was attempted.

CTest commands (the initial batch also ran four automatic prefix cleanup tests,
while the FEX recheck ran two):

```
RAVEN_PREFIX_SETUP=1 /tmp/raven-ctest-lock.sh mtld3d-dxmt-regression \
  ctest --test-dir /tmp/raven-mtld3d/build -j1 \
  -R '^darwin_dxmt_(triangle|compute)(_fex)?$' --output-on-failure \
  --output-junit /tmp/raven-mtld3d/dxmt-regression.xml
RAVEN_PREFIX_SETUP=1 RAVEN_FEX_MODULE_ROOT=/tmp/raven-mtld3d/fex-modules \
  /tmp/raven-ctest-lock.sh mtld3d-dxmt-fex-recheck \
  ctest --test-dir /tmp/raven-mtld3d/build -j1 \
  -R '^darwin_dxmt_(triangle|compute)_fex$' --output-on-failure \
  --output-junit /tmp/raven-mtld3d/dxmt-fex-recheck.xml
```

Raw evidence is private: `probe.log`, `fixture64.log`, `import64.log`,
`fixture32.log`, `prefix{32,64}/drive_c/window.txt`, the two CTest logs/XML,
and build/configuration logs, all under `/tmp/raven-mtld3d`.

Remaining implementation is substantial: provision/qualify the PE runtime
dependencies, integrate real Cocoa presentation, implement recursive/asynchronous
WoW64 address translation, then rerun strict readback and screen tests. This
branch is a starting point for that work, not completion of the requested task.
