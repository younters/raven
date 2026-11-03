# macOS resize design investigation

## Measurement contract

Measure host frame change → desktop resize dispatch → guest placement completion
→ producer frame publication → host presentation. A new-size GDI bitmap may
contain copied old pixels; it is not proof that Chromium repainted. Cocoa trace
entries use monotonic milliseconds, comparable across host processes. Layer
submission is also not a measurement of WindowServer scanout.

Baseline and experiment logs belong in `/Volumes/Storage/Projects/raven-wip/resize`.
Do not attribute seconds to a timer merely because one exists in the source.

## Alternatives (evaluated before implementation)

1. **Host scaling of retained content.** Let the content layer fill the current
   host bounds immediately, replacing its contents only on completed presentation.
   This decouples visible geometry from guest latency and works for CGImage and
   IOSurface GDI output. It costs no guest allocation and avoids exposing uncovered
   areas. Text is temporarily stretched, and scaling alone cannot accelerate a
   delayed WM_SIZE or a stalled foreign producer. Hosted graphics child layers
   require separate treatment; stretching their parent is not a swapchain resize.

2. **Authoritative geometry transactions.** Coalesce host changes at each desktop
   poll, then run the existing window-manager placement transaction. Track requested,
   delivered and committed geometry explicitly; never use presentation completion
   as permission to deliver the next size. This replaces adaptive acknowledgement
   waits, quiet periods and bitmap-driven geometry rollback. It preserves the
   window manager's changing/NCCALCSIZE/changed/default SIZE ordering. Full
   generation propagation through foreign GDI and DComp is a larger protocol
   change: dimensions alone cannot distinguish A→B→A or an application veto.
   Risk: without an explicit placement acknowledgement, host and guest can disagree
   when WM_WINDOWPOSCHANGING changes the request.

3. **Synchronous drawable resize with lazy guest rebinding.** Resize CAMetalLayer
   with the host and let old guest swapchains catch up. Appropriate for native
   graphics presentation, but does not fix Chromium's foreign GetDC/BitBlt route.
   Changing drawable extent behind a producer risks mismatched render targets,
   resource lifetime and frame fences. It requires qualification across DXMT,
   DComp and Vulkan, and is unsuitable as the sole GDI solution.

4. **Windows modal size/move lifecycle.** Send ENTERSIZEMOVE/EXITSIZEMOVE around
   native edge tracking and let Chromium optimize intermediate sizes. This is a
   semantic improvement for actual dragging, but maximize is not a modal resize.
   Throttling SIZE within that loop can worsen final-size latency. AppKit's nested
   loop also risks starving the guest unless pumping is explicitly integrated.
   This cannot alone explain or solve maximize latency.

## Decision gate

Prefer a geometry transaction design combined with retained-content scaling if
measurement confirms delayed notification or stale-frame geometry feedback.
Do not implement a new timing heuristic. Preserve guest veto/min-max behavior and
SW_MAXIMIZE/restore semantics. Keep producer readiness separate from geometry;
changing the desktop's initial-paint policy requires independent evidence.

The following measurement informed the implementation; before/after application qualification remains pending.

## Baseline observation and selected implementation

One clean-profile Chrome software-rendering run expanded the Cocoa frame from
924×1030 to 1878×1050. At monotonic 13648093.563 ms the host changed size;
dispatch followed at 13648093.967, placement began at 13648264.074 and ended at
13648427.826. The foreign producer allocated 1870×1046 at 13648619.040;
import occurred at 13648635.368 and Cocoa presentation callback at 13648636.552.
Thus dispatch cost 0.404 ms, dispatch-to-placement 170.107 ms, placement
163.752 ms, and host-to-producer-backed presentation callback 542.989 ms. New-size root
presentation callbacks began after 226.861 ms but contained preserved content. This is one
sample under memory pressure, not a reproduction of a multi-second maximize.

Select (1) plus the host transaction portion of (2): retain and scale content,
coalesce once per poll without paint-dependent waits, and explicitly acknowledge
host geometry after window-manager placement (including a guest veto). Number
requests so an older placement cannot undo a newer host change. Keep the existing
Windows placement path and its message behavior; no new message order is invented.
Do not modify producer allocation or scheduler policy on the basis of this sample.
Remove adaptive paint acknowledgements, quiet/stillness timers and crossfades.
This improves geometry responsiveness and removes feedback, but does not claim
to solve the observed 170 ms scheduling delay or accelerate Chromium rendering.

Steam measurement was interrupted because another agent launched Steam from the
main checkout. Only this worktree's processes were stopped. Application runs must
remain suspended while that session exists.

## Remaining qualification

The measured endpoint is the Cocoa `present` callback, before its contents update,
not a GPU fence or screen scanout. The diagnostic work-area expansion is not
a complete SW_MAXIMIZE/restore measurement. The normal GPU Chrome run was stopped
after memory availability fell from 56% to 27%; it supplies no baseline.
The software Chrome sample does not establish the reported seconds-long bottleneck.

This change leaves existing desktop pending-paint and blank-client policies intact.
A new-size root bitmap may still contain copied old pixels before a foreign producer
redraws. Hosted DXMT/DComp geometry and modal ENTERSIZEMOVE/EXITSIZEMOVE integration
are unchanged. No claim of complete Chromium repaint acceleration or blank-free
foreign resize is made until producer readiness and application runs are qualified.

Per the owner's follow-up, the main-checkout Steam/Portal 2: Community Edition
gameplay session must remain untouched until approximately 23:30 local time.
Baseline and after Steam/Chrome runs are pending if that session outlasts the
implementation and automated test work.

## Verification (2026-11-02)

Baseline source: `27cfa1ea910349ce8ca228f8e22b37a023daed79`. Isolated
RelWithDebInfo/Ninja build: `build-resize`, with external component paths read
from the supplied cache. Builds used `nice -n 10`, two jobs and `-k 0`.
Both `raven` and `raven-fex-host` were relinked after the final refinement.

Requested broad CTest regex (`darwin|cocoa|gdi|dcomp|frame|window|nonclient|desktop`),
serial, with prefix setup: **349 passed, 4 skipped, 8 failed**, 260.74 seconds
(includes CTest prefix cleanup fixtures). Cocoa policy/input/hidden-surface,
GDI presenter, native/FEX foreign GDI, window/frame/nonclient and DComp cases
passed. The extended Cocoa policy test verifies coalescing, stale-paint geometry
and guest-adjusted acknowledgement. The pure state contract covers one placement
in flight, pending-size replacement and stale generations including A→B→A.

After the final one-placement-in-flight refinement, focused resize/Cocoa/GDI
and failure rechecks: **22 passed, 7 failed**, 10.96 seconds (including cleanup).
All new and directly affected tests passed. FEX tessellation cleared on recheck.
Remaining failures:

- `darwin_d3d_compile`, `darwin_d3d_compile_local`: compiler lookup, exit 10.
- `darwin_dxmt_pacing`: minimum-duration pacing assertion.
- `native_darwin_runtime`: host TSC / monotonic nanoseconds assertion.
- `native_darwin_ole_picture_bounds`: unimplemented CreateStreamOnHGlobal.
- `native_darwin_desktop_dpi_x64`, `_x32`: guest result mismatch.

These failures were not weakened or fixed here; an unchanged-source baseline
recheck has not been performed, so they must not all be labeled proven pre-existing.
Full logs: `resize/ctest.log`, `resize/ctest-focused.log` under the scratch directory
specified above. Final availability check: other Raven processes still running,
29% memory free. Steam/Chrome baseline and after qualification remains pending.
