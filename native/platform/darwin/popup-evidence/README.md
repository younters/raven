# Owned popup GPU content lifetime

Base: `65b8ff6648e7777a021667edfbfa8d6030ae6678`.

`CocoaPresentation::State::popup` replaced an existing regular host window (or
one attached to a different owner) with `forget(handle)`. That path also erased
`hidden_host_surfaces`, including the CAMetalLayer referenced by the HWND's swap
chain. Submitted DirectComposition images remained attached to the detached old
layer. With no new Present, the replacement popup showed only its GDI backing.
The reverse transition already used `forget(handle, true)`.

Use the same replacement semantics in both directions: detach the view, preserve
the per-HWND layer and its submitted images, create the new host, and reattach.
An ownership/presentation change does not destroy the Windows HWND or its swap
chain. Actual destruction still releases the surface.

Regression coverage:

- The Cocoa transition test checks layer identity and retained submitted content
  across regular-to-owned and owned-to-regular transitions.
- `darwin_dcomp_popup-transition-native` and
  `darwin_dcomp_popup-transition-fex`
  submit a green composition image over a red GPU-hosted owner, remove and restore
  the owner, then check screen pixels without a new Present or Commit. Existing
  clip, alpha, retained commit and resize checks run afterward.
- Original Cocoa implementation: fails with “converting a regular host to an
  owned popup lost the submitted image”.
- Original runtime with the new PE fixture: stage 1 renders green; stage 2 fails
  because the green image disappears after restoring ownership.

Chrome qualification uses an APFS clone of the fixture prefix at
`/tmp/raven-chrome-popup`, RAVEN_PARALLEL=1, and the supplied DXMT/FEX component
roots. Both original-runtime engines eventually rendered a fresh-profile restore
bubble correctly. Early captures were black before GPU startup completed.
The exact supplied Gmail/Images bleed-through and doubled border were not
reproduced. The deterministic regression proves the layer-lifetime defect;
it does not prove that every artifact in the supplied Chrome capture has the
same cause. No UpdateLayeredWindow or GDI alpha behavior was changed.

Build, test logs and window captures are retained in
`/tmp/raven-popup-evidence` and `build-popup/native/dcomp-popup-transition-*`.

## Screen evidence

Captured with `screencapture -l`, stage 2 of the same owned-popup fixture:

- [Original runtime: submitted image lost](before-transition.png)
- [Fixed runtime: Rosetta](after-transition-native.png)
- [Fixed runtime: FEX](after-transition-fex.png)

The green area is the popup's own submitted image, not the GPU owner's red
content. The test measures its bounds and position before and after ownership
changes, then checks clipping, alpha blending, detach, and resize pixels.

## Validation

RelWithDebInfo build, Ninja `-j8`, component paths and font hashes copied from
the supplied build-macos cache after libpng was restored. The 346 fixture targets registered at the time were built. Disk reserve remained above 8 GiB (initial free space was
8.3 GiB, later 10–16 GiB).

All CTest commands use `/tmp/raven-ctest-lock.sh`:

- Targeted selection `dcomp|dxmt|window|popup|cocoa`, `-j4`: 149 passed,
  9 failed out of 158 including cleanup fixtures. Both new DComp pixel tests
  passed. This initial run applied RAVEN_PARALLEL=1 globally; eight existing
  FEX shader/draw tests failed with only clear-color pixels. The remaining
  failure was a Cocoa assertion checking CALayer attachment before AppKit
  committed its layer tree; it now checks NSView attachment directly.
- One serial rerun of failures with each test's normal scheduling settings:
  all 9 failed tests and 9 cleanup fixtures passed. The new DComp tests retain
  explicit RAVEN_PARALLEL=1 in their own CTest environment.
- One full suite with RAVEN_PREFIX_SETUP=1, `-j4`: **1,581 passed, 11 failed,
  8 skipped, 1 disabled**, 205.68 seconds. All DComp tests, including both new
  engine regressions, and the corrected Cocoa transition test passed.
  `native_vulkan_cocoa_present` failed its four-quadrant screen comparison
  (it passed in the targeted run). Ten failures reported “cannot open PE file”:
  four x87sidecar cases, four context-groups engine/width cases, and two mtld3d
  runtime cases. Their five fixture targets were registered after component
  restoration and were missed by the earlier target enumeration. Those targets
  have since been built successfully; no extra test runs were made.

Patched-build Chrome captures use the cloned fixture's original profile and
normal sandboxing, with RAVEN_PARALLEL=1. Both restore bubbles display their
explanation, checkbox, usage-statistics link, close control and Restore button:
[Chrome FEX](chrome-after-fex.png), [Chrome Rosetta](chrome-after-native.png).
