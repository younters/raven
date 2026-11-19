# Cocoa cursors and Retina rendering

The host remains arm64; the Windows program and its callbacks remain x86_64 under Rosetta. No debugger is needed for this work.

## Cursor delivery

Pointer motion hit-tests the child window, sends `WM_SETCURSOR` synchronously, then queues the mouse move. Capture suppresses automatic cursor selection. `DefWindowProc` offers the message to parents first, preserving the original target in `wParam`; an ancestor's default procedure leaves the child's cursor alone. The target uses its class cursor in the client area and stock sizing/arrow cursors elsewhere. A class with no cursor leaves selection to the application, as Windows does.

The optional presentation `cursor` callback receives the effective handle, `ShowCursor` count, and a borrowed custom image. Cocoa maps stock handles to native cursors and converts RGBA/AND/XOR bitmaps and hotspots into `NSCursor` images. `CreateCursor`, colored and monochrome `CreateIconIndirect`, resource `LoadCursorW/A`, and cursor `LoadImage` are supported. Shared resource cursors are cached. Custom hotspots scale with their images.

Cocoa applies guest cursors only over guest content. Native title bars, other applications, and the existing native resize-edge cursors retain control. Visibility has a balanced AppKit hide count, independent of pointer confinement. WAIT/APPSTARTING and HELP use static system-symbol approximations. AppKit cannot invert destination pixels; monochrome XOR-invert pixels are approximated by opaque white. Animated cursors and `.cur` files loaded through `LR_LOADFROMFILE` are not implemented.

## Coordinate contract

`backing_scale` reports device pixels per Cocoa point; `render_scale` selects the program's coordinate scale. Awareness comes from the executable manifest or the process awareness setters. Legacy `dpiAware` and ordered modern `dpiAwareness` values are parsed; the modern declaration takes precedence.

* Unaware programs use 96 DPI, one guest pixel per Cocoa point, and nearest-neighbor Retina bitmap scaling.
* Aware programs on a 2x output use 192 DPI and one guest pixel per backing pixel. Their frames, screen size, input, frame insets, popup anchors, clipping rectangles, and Metal surface rectangles use device pixels. Cocoa converts only at its points boundary. Metal layers retain device-pixel drawable sizes.
* `GetWindowRect`/`SetWindowPos` and mouse coordinates use device pixels. Cocoa `WINDOWPLACEMENT` is persistent workspace geometry in points: both getter and setter convert, preserving size across launches and avoiding repeated doubling. This is an intentional macOS workspace policy, not full Windows DPI coordinate virtualization.

DPI reaches `GetDpiForWindow`, `GetDpiForSystem`, process/monitor queries, `GetDeviceCaps(LOGPIXELSX/Y)`, system metrics, default desktop/GUI/fixed fonts, and the W/explicit-DPI system-parameter fonts and metrics. The x64 guest's fixed-96 DPI shortcuts are bypassed only where needed on Darwin. Frame borders are calculated at 96 DPI and scaled once, avoiding a second multiplication inside the imported ReactOS border calculation. `LoadImage` honors requested resource bitmap dimensions, so 192-DPI toolbar icons have the requested size.

On first showing an aware top-level window, Raven applies the initial Cocoa scale to an unowned captioned window's startup bounds (or its restored workspace placement) and sends `WM_DPICHANGED` with 192 in both words and a guest `RECT`. Owned dialogs already derive their extents from scaled fonts. This initialization lets applications refresh toolbar, tab and status-bar layout after their controls exist. The initial notification is also delivered to system-aware applications for compatibility with their initialization paths.

The stock fixture ZIP is Notepad++ **8.7.6**. Its embedded manifest says `system, unaware`, not PerMonitorV2. Its Windows 10 detection probes `RtlGetNtVersionNumbers`; without that export, it falls back to a font path that scales the tab font again and omits `SystemParametersInfoForDpi`. The runtime now supplies that version query. A separate test executable carries a PerMonitorV2 manifest. Testing a temporary Notepad++ copy with only the modern manifest value changed to PerMonitorV2 also exercises that path; the original download is unchanged.

## Scope still open

The current scale is chosen from the main output at launch and applies to the process. Moving between outputs with different backing scales does not yet deliver a new monitor DPI transition. Mixed-awareness windows in one process, full thread-sensitive coordinate/GDI virtualization, and PMv2 child before/after-parent DPI notifications remain open. Thread DPI contexts are remembered and the system-font/system-DPI queries respect an unaware override, but this is not full mixed-mode hosting. ANSI `SystemParametersInfoA` remains the pre-existing stub. Some theme drawing sizes and menu ornament/padding dimensions still use classic pixel constants. Resource bitmap enlargement supplies the requested dimensions but cannot invent higher-resolution artwork. Metal geometry is updated; no new Vulkan/DXMT implementation is included.

## Verification

Build normally, then run tests serially:

```sh
cmake --build build
ctest --test-dir build -j1 --timeout 120 -R native_darwin_cursor_dpi --output-on-failure
ctest --test-dir build -j1 --timeout 120 --output-on-failure
```

The integration test runs three real Windows fixtures: programmatically DPI aware, unaware, and manifest PerMonitorV2. It checks parent cursor overrides and capture, stock/custom/resource cursors, hotspots and masks, `ShowCursor`, bitmap resizing, DPI queries on window and memory DCs, default GUI and system-parameter fonts, an unaware thread override, explicit-DPI frame adjustment, the initial notification, window extent scaling, and placement round trips. The fake presentation reports a 2x output; it does not require AppKit UI automation. Notepad++ supplies the real Cocoa visual/input check. `RAVEN_TRACE_COCOA=1` includes cursor-handle/count/image changes as well as frame sizes.

At the implementation baseline (`c9c7e1a`), this checkout registered 253 tests: 195 passed, 57 failed, and one was skipped (CTest prints 77%). The final 254-test serial run passed the new integration test and had 195 passes, 58 failures, and the same skip. The sole additional failure was `native_dll_lifecycle`: its 300 ms `loop.dll` startup budget expired before `base.dll` initialization (`base_state=0`); the separate fault case retained the expected `1125` state. Three isolated reruns also hit the short startup budget. No loader or guest-agent implementation changes are included here.

Real Notepad++ verification: the 1100x700 saved placement produces a 2200x1400 guest frame in a 1092x698-point native window, including the native title bar. A clean quit/relaunch preserves the saved 1100x700 placement and returns to 2200x1400 rather than doubling it again. API traces show 192 for both display axes and system/window DPI, the initial `WM_DPICHANGED` packs `0x00c000c0`, and pointer traces show stock `0xfa01` (I-beam) over Scintilla and `0xfa00` (arrow) over the toolbar. The temporary PerMonitorV2 Notepad++ copy also renders at 2200x1400.

## Child controllers and foreign GDI windows

A child Raven controller inherits its creator's current desktop DPI in host launch metadata (`RAVEN_DESKTOP_DPI`), independently
of an explicit Windows environment block. Its own awareness declaration is retained, but its desktop coordinates and render scale
remain in the creator's space. This is the existing single-space desktop policy, not complete Windows mixed-awareness virtualization.
The x86 legacy awareness setter goes through the same Darwin handler as the newer awareness APIs.

Steam's unaware 32-bit process and aware 64-bit CEF helper previously chose different scales. A captured black main-window run
published a 7032x4520 foreign client while its producer retained a 1x1 surface; CEF then stopped in its GDI failure diagnostic.
With inherited DPI, the signed-in Store rendered through CEF's separate software/GPU process: GetDC/BitBlt/ReleaseDC published
1758x1130 and 1756x987 shared frames with advancing sequences, and the captured Cocoa window contained Store artwork and navigation.
The user's original prefix was not modified. The private copy automatically signed in using saved credentials and was stopped after
capture; whether it displaced the original account session was not established.

`native_darwin_desktop_dpi_x64` and `_x32` launch a helper choosing the opposite awareness at both 96 and 192 DPI. They also acquire
a foreign DC at 1x1, resize the owner to 17x11, reacquire and draw a nonuniform page, then check the owner surface's pixels. Both pass.
Foreign DCs already allocate from the owner's published client pixels and recreate their bitmap when that extent changes.
