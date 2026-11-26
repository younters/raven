# GDI presentation

Top-level desktop bitmaps use CPU-mappable IOSurface storage. `SharedPixels(size, width, height)` keeps Raven's exact `width * 4`
stride and asks for at least the page-rounded bitmap size; IOSurface rounds the allocation to the host's 16 KiB page. The
surface's descriptor is a process-local token: `NativeProcess::map_shared` recognizes it, reserves the whole host-page extent at a
64 KiB-aligned guest address through the agent, and maps the IOSurface pages over that reservation with
`mach_vm_remap(copy=false)`. The controller's host alias keeps the same IOSurface, so no pixels pass through the agent's service
channel. Ordinary DIB sections, including ones at nonzero 4 KiB offsets, keep their file mappings.

GDI draws incrementally into a bitmap whose address does not change between presents. Handing that writable surface to a CALayer
would race WindowServer, and rotating it would mean copying unchanged pixels. Instead, Metal reads the IOSurface's pages through
`newBufferWithBytesNoCopy` and renders into a pool of three output surfaces. The shader reads the tight GDI stride directly (Metal
textures would need 16-byte row alignment), turns BGRX into opaque BGRA so that GDI's unused alpha never makes a window
transparent, and tags the output sRGB to match the CGImage path on wide-gamut displays.

The cooperative guest is stopped during presentation and resumes only once the GPU has read its bitmap. WindowServer reads only
completed output surfaces. The pool skips the surface currently in the layer, surfaces with pending Core Animation submissions and
surfaces `IOSurfaceIsInUse` reports as read. When all three are busy, intermediate updates are coalesced, and the desktop poll
presents the latest one once a surface is free, including the last update of a program that then stays idle.

`contentsRect` crops the native title bar and borders without a cropped copy. Raven's presentation callback carries no damage
rectangle, so the GPU renders the whole frame on each update; partial GDI updates upload nothing. Retained contents scale with the
host bounds during a resize (see [RESIZE.md](RESIZE.md)).

## Fallbacks and switches

The original CFData/CGImage snapshot path remains for `RAVEN_COCOA_PRESENT=copy`, for Metal failures, temporary menu buffers and
glass-composited spans, and for `RAVEN_PARALLEL=1`: until GDI has a producer fence, the GPU cannot borrow a bitmap that another
Windows thread may be writing. `RAVEN_COCOA_PRESENT_STATS=1` reports Metal frames, fallbacks and pool pressure when a window closes.
`RAVEN_COCOA_DUMP=<directory>` writes each presented frame as a PPM image, read through the source surface's CPU mapping.

## Tests

`native_darwin_gdi_presenter` checks guest writes into the last byte of odd-width allocations, pixel readback (channel order,
opacity and orientation), incremental updates, exclusion of in-use outputs, and the retry of the final update after the pool runs
out. `native_darwin_runtime` and `native_compatible_bitmap_paint` cover the shared mapping.

```sh
ctest --test-dir build-macos -j1 -R 'native_darwin_(runtime|gdi_presenter)$' --output-on-failure
```
