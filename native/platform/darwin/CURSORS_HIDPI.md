# Cursors and Retina rendering

## Cursor delivery

Pointer motion hit-tests the child window, sends `WM_SETCURSOR` synchronously, then queues the mouse move. Capture suppresses
automatic cursor selection. `DefWindowProc` offers the message to parents first, keeping the original target in `wParam`; an
ancestor's default procedure leaves the child's cursor alone. The target uses its class cursor in the client area and the stock
sizing and arrow cursors elsewhere. A class with no cursor leaves the choice to the program, as Windows does.

The presentation `cursor` callback receives the effective handle, the `ShowCursor` count and a borrowed custom image. Cocoa maps
stock handles to native cursors and converts RGBA and AND/XOR bitmaps and their hotspots into `NSCursor` images. `CreateCursor`,
color and monochrome `CreateIconIndirect`, resource `LoadCursorW`/`LoadCursorA` and cursor `LoadImage` are supported. Shared
resource cursors are cached, and custom hotspots scale with their images.

Cocoa applies guest cursors only over guest content; native title bars, other applications and the native resize edges keep their
own. Visibility keeps a balanced AppKit hide count, independent of pointer confinement.

## Coordinate contract

`backing_scale` reports device pixels per Cocoa point; `render_scale` selects the program's coordinate scale. Awareness comes from
the executable's manifest or the process awareness setters. Legacy `dpiAware` and ordered modern `dpiAwareness` values are parsed,
and the modern declaration takes precedence.

- Unaware programs use 96 DPI, one guest pixel per Cocoa point, and nearest-neighbor scaling on Retina displays.
- Aware programs on a 2x display use 192 DPI and one guest pixel per backing pixel. Their frames, screen size, input, frame insets,
  popup anchors, clipping rectangles and Metal surface rectangles are in device pixels; Cocoa converts only at its own boundary.
- `GetWindowRect`, `SetWindowPos` and mouse coordinates use device pixels. `WINDOWPLACEMENT` is stored in points, so a saved
  placement keeps its size across launches.

The DPI reaches `GetDpiForWindow`, `GetDpiForSystem`, the process and monitor queries, `GetDeviceCaps(LOGPIXELSX/Y)`, system metrics,
the default desktop, GUI and fixed fonts, and the explicit-DPI system parameters. Frame borders are computed at 96 DPI and scaled
once. `LoadImage` honors requested resource bitmap sizes, so 192-DPI toolbar icons have the size asked for.

When an aware top-level window is first shown, Raven scales an unowned captioned window's startup bounds (or its restored placement)
and sends `WM_DPICHANGED` with the effective DPI and the new `RECT`, so the program can lay out its toolbars, tabs and status bars again once its
controls exist. System-aware programs receive it too, for their initialization paths.

A child Raven process inherits its creator's desktop DPI (`RAVEN_DESKTOP_DPI` in its launch metadata), so a helper with a different
awareness still draws in its creator's coordinate space. Foreign DCs use the owner's published client size when allocating their bitmap.

## Tests

`native_darwin_cursor_dpi` runs three Windows fixtures, programmatically aware, unaware and manifest Per-Monitor v2, against a
presentation that reports a 2x display, so it needs no GUI session. It checks cursor overrides and capture, stock, custom and
resource cursors, hotspots and masks, `ShowCursor`, bitmap scaling, DPI queries on window and memory DCs, default fonts, an unaware
thread override, explicit-DPI frame adjustment, the initial `WM_DPICHANGED`, extent scaling and placement round trips.
`native_darwin_desktop_dpi_x64` and `_x32` start a helper with the opposite awareness at 96 and 192 DPI and draw through a foreign
DC before and after the owner resizes. `RAVEN_TRACE_COCOA=1` logs cursor changes and frame sizes.
