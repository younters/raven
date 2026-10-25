# GDI presentation on macOS

Normal top-level desktop bitmaps use CPU-mappable IOSurface storage. `SharedPixels(size, width, height)` retains Raven's exact
`width * 4` stride and requests at least the existing page-rounded bitmap size. IOSurface rounds the allocation to the host's page
size. Its descriptor is a process-local allocation token: Darwin `NativeProcess::map_shared` recognizes it, reserves the complete host-page extent at a 64 KiB-aligned address through
the Rosetta agent (as the previous descriptor mapping did), and uses `mach_vm_remap(copy=false)` to map the IOSurface pages over that reservation. The controller's fast host
alias retains the same IOSurface. No pixel transfer through the guest service channel is involved. Ordinary DIB sections, including
nonzero 4 KiB offsets, keep their existing file mappings.
The desktop's existing spare-surface copy when allocating a new bitmap size remains; presentation itself performs no CPU copy on
the IOSurface path.

GDI drawing is incremental and its bitmap address persists across presents. Directly placing this writable source in a CALayer would
race WindowServer, and rotating the source would require copying unchanged pixels to preserve GDI contents. Instead, Metal reads the
IOSurface's pages through `newBufferWithBytesNoCopy` and renders into a three-surface output pool. This is zero CPU upload, rather than
a promise that the GPU never reads and writes a complete frame. The buffer shader also avoids Metal's 16-byte texture-row alignment
requirement: tight GDI strides such as 68 bytes remain valid. It converts BGRX to BGRA with alpha 255; GDI's unused alpha never makes
Windows UI transparent. Outputs carry explicit sRGB colour-space metadata to match the CGImage backend on wide-gamut displays.

The cooperative guest is blocked during presentation and resumes only after the GPU finishes reading its bitmap. WindowServer reads
only completed output surfaces. The pool excludes the current model-layer contents, pending CA submissions (an IOSurface use count
released by transaction completion), and surfaces for which `IOSurfaceIsInUse` reports a reader. A full pool coalesces intermediate
updates without a CPU copy. The desktop poll retries the latest source after pumping CA completions, including the final update when
the program sends nothing further. A resized pool releases its references; CA retains old outputs for the existing 60 ms crossfade.

`contentsRect` crops native title bars and borders without creating a cropped image. The existing `contentsScale` selection for aware
and unaware programs, nearest magnification, bottom-left gravity, geometry and adaptive resize pacing remain in Cocoa. No texture
upload occurs for partial GDI updates. Raven's presentation callback supplies no damage rectangle, so the GPU currently renders the
whole frame. `RAVEN_COCOA_DUMP` reads the CPU-mapped source IOSurface through the existing pixel span.

The desktop's existing pending-paint deadline remains 250 ms, or 2000 ms before initial presentation
(`NativeWindows::present`). This is distinct from Cocoa's adaptive 10–100 ms wait for a resize response and delivery of the final
size after 16 ms of stillness. Metal adds no debounce timer. Pool-pressure retries use the event pump and drain their temporary
Metal/CA objects in an autorelease pool. They depend on CA/WindowServer releasing an output: these timers do not establish a hard
deadline for GPU execution or WindowServer completion.

`RAVEN_COCOA_PRESENT=copy` keeps descriptor allocations and the original CFData/CGImage snapshot path for comparisons. Metal failure,
temporary menu buffers and glass-composited spans also use snapshots. Parallel guest mode uses snapshots until GDI supplies a producer
fence; a GPU read cannot safely borrow a bitmap while another Windows pthread writes it. Synthetic menus still use the established
popup path. These exceptions are not zero-copy. `RAVEN_COCOA_PRESENT_STATS=1` reports Metal frames, unsupported input fallbacks and
pool pressure at window teardown, and identifies the first shared source in the log.

Contracts cover guest writes into the last byte of odd-width allocations, pixel readback (BGR, opacity and orientation), incremental
updates, in-use buffer exclusion, and retrying the final update after pool exhaustion:

```sh
/tmp/raven-ctest-lock.sh gdi ctest --test-dir build -j1 --timeout 120 \
  -R 'native_darwin_(runtime|gdi_presenter)$' --output-on-failure
```

See `gdi-presentation-results.json` for the measured comparison and validation results.

On an M4 Pro running macOS 27.2, five interleaved copy/Metal pairs per scenario produced these median CPU percentages
(100% means one core):

| Scenario | Copy host | Metal host | Copy guest | Metal guest | Median paired host+guest change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Blinking caret | 3.85 | 2.60 | 0.29 | 0.33 | -0.98 percentage points |
| Scrolling | 45.57 | 32.66 | 14.93 | 14.75 | -13.47 percentage points |
| Scripted resize | 25.68 | 21.95 | 3.14 | 3.26 | -3.76 percentage points |

Each measurement lasted 10 seconds, with per-process `top` output and an 8-second host `sample`. CPU values use process user+system
counter deltas, converted from this host's Mach ticks with `mach_timebase_info` (125/3), checked against `top`. The user's Steam
session remained active: aggregate background Raven CPU ranged from approximately 82% to 282%, recorded for every run. Idle pair 3
favoured copy; all five scrolling and resize pairs favoured Metal. These short, loaded measurements establish a CPU comparison,
not an energy measurement. `powermetrics` required root and was skipped. The JSON includes every pair's deltas and background CPU.

The CPU improvement trades for persistent buffers and Metal resources. Median host physical footprint increased from 133 to
240 MiB at idle, 132 to 249 MiB while scrolling, and 196 to 296 MiB during resize. These samples precede the final autorelease-scope
correction and include the whole host process, not only surface storage. This implementation does not claim a memory reduction.

The copy and Metal full serial suites each registered 284 tests: 272 passed, 8 failed, 3 skipped, 1 disabled, with identical failure
sets. `native_compatible_bitmap_paint` exposed an IOSurface guest-remap regression during development: a 4 KiB arbitrary reservation
did not preserve the original mapping's 64 KiB alignment or the entire 16 KiB host-page allocation. Reserving through the existing
descriptor mapper with `IOSurfaceGetAllocSize` corrected it; both modes then passed it individually and in the full suites.
After the final autorelease-scope correction, the compatible-bitmap, Darwin runtime and Metal presenter contracts all passed again
under the shared test lock (3 tests, 3.91 seconds).

Observed Notepad++ frames had correct opaque colours, Retina text, title-bar cropping, scrolling and File-menu presentation.
No obvious tearing was observed; there was no high-speed recording or exhaustive context-menu coverage. Firefox glass/title-bar
composition remains a snapshot fallback and needs separate integration work before claiming CPU-copy-free presentation there.
