# Chromium FEX black-content investigation (2026-10-31)

Implementation: `fix/fex-chromium-presentation`, based on `30b209eb` and rebased
onto `f9b35048`. Runtime fix: `3253955c`; cross-process GDI fixture: `e7ad0fc1`.

## Root cause and fix

Chromium's software output process calls `GetDC` for a browser-owned HWND.
`NativeWindows::foreign_dc` maps the shared pixel file after its 4096-byte
header. FEX rejected every file offset not divisible by 16384. The exception
terminated the compositor helper before it could BitBlt or publish a frame.
Chrome restarted the helper repeatedly and the browser window remained black.
The pre-fix log includes:

```
raven: guest thread 58 failed in GetDC: FEX shared mapping offset must be host-page aligned
GPU process exited unexpectedly: exit_code=1
```

`FexProcess::map_shared` now maps the enclosing Darwin pages, returns the
Windows-page offset within the mapping, and remembers the reservation base for
unmapping. The preceding header pages are excluded from guest read/write
permissions. Mapping-size/offset validation and cleanup on failure remain in
place. No external FEX module patch or component mutation was needed.

The new `native_shared-offset_fex` test exercises 4, 8, 12 and 16 KiB offsets,
shared file writes, header protection, and teardown. The new
`darwin_gdi_foreign_present_native/fex` tests launch a real PE producer in a
separate process. They verify every pixel after GetDC/BitBlt/ReleaseDC and after
a foreign-window resize. Both use parallel scheduling. The fixture fails on
the unmodified parent build with the same offset rejection and passes with the
fix on both engines. It does not require a display or a graphics driver.

## Other suspects

- `f8fb5c27-c6b3-4f75-a4c8-439af2ef564c` is **ID3D11Texture1D**, as defined in
  the installed DirectX headers. A Texture2D rejecting that query is expected;
  it is not evidence of a failed keyed-mutex query.
- `f9b35048` already resolves the duplicate `ImmGetProperty` registration by
  retaining the new common implementation and removing the Darwin duplicate.
  It also removes the duplicate GetCurrentProcessExplicitAppUserModelID fallback.
  ApiRegistry throws on duplicate registration, aborting child startup. The
  supplied `/tmp/chrome-user-session.log` was read without modification.
- The upstream merge changes GDI masks/blits and adds an UpdateLayeredWindow
  success stub. The reproduced software presentation uses BitBlt, not that
  stub. This change does not implement layered-window alpha presentation.
- Hosted placement still paints the root on acquisition; placement during a
  root repaint correctly avoids recursion. Normal GPU Chrome visibly renders,
  and the existing local/foreign/HiDPI/popup DComp pixel fixtures exercise this
  route. No change to `place_hosted` or the graphics unix-call guards was needed.
- `0xFFFF7001` is Crashpad's `kTerminationCodeCrashNoDump`: the client
  self-terminated because the crash handler did not respond. Raven prints the
  guest termination status; it does not synthesize this code. The parent's log
  does **not** establish the original browser fault that led to Crashpad. Child
  startup failures and the independently reproduced GetDC failures must not be
  substituted for evidence of that particular original fault.
  Source: https://chromium.googlesource.com/crashpad/crashpad/+/c4cdec3d72a2d2abad84a98bc0bcf525333b3280/util/win/termination_codes.h

## Application evidence

All Chrome runs use APFS clones of the fixture prefix under
`/tmp/raven-chromium-investigation`, RAVEN_PARALLEL=1, an isolated Chrome profile,
and the included local HTML page. Normal qualification keeps sandboxing enabled.
The initial no-sandbox investigation is separate. Common guest arguments:

```
--disable-extensions --disable-background-networking --disable-component-update
--no-first-run --no-default-browser-check --enable-logging --v=1
--remote-debugging-port=9337 --user-data-dir=C:\raven-chrome-test
file:///Z:/tmp/raven-chromium-investigation/page.html
```

The software comparisons additionally pass `--disable-gpu`. Local HTTP access
to the debugging port did not succeed; chrome://gpu was not inspected.

| Evidence | Result |
|---|---|
| [Software FEX before](software-fex-before.png) | Black window; repeated GetDC failures and helper restarts |
| [Software FEX after](software-fex-after.png) | Page and Chrome toolbar render; successful BitBlt calls |
| [Software Rosetta](software-native.png) | Page and toolbar render |
| [GPU FEX](gpu-fex.png) | Page and toolbar render |
| [GPU Rosetta](gpu-native.png) | Page and toolbar render |
| [Steam startup readback](steam-startup-readback.png) | Rendered 700x440 surface belonging to “Sign in to Steam”; loading-user-data screen |

Chrome evidence is actual `screencapture -l` window capture. Early screenshots
before the first GPU frame were black and are not evidence of a hosted-layer
failure. Steam used a fresh `cp -cR /tmp/raven-rosetta-steam-pristine` copy,
launched normally under FEX after a preliminary flagged startup stalled.
Screencapture of the Steam window failed (“could not create image from window”),
so Steam evidence uses `RAVEN_DXMT_CAPTURE_DIRECTORY` IOSurface readback. No
credentials were entered. **The finished Steam login form and logged-in main
window were not verified.** The user's running Steam session and
`/tmp/raven-steam-login-fex` were not touched. Only owned process groups were
stopped.

## Build and verification

RelWithDebInfo, Ninja, -j8. FONT_HASHES, PNG_LIBRARY_RELEASE,
PNG_PNG_INCLUDE_DIR and all requested component, DXMT, Mullion, SQLite, font
and Skia settings were copied from the supplied build-macos CMakeCache.
RAVEN_FEX_MODHOST=ON and the supplied RAVEN_FEX_MODULE_ROOT were selected.
All 344 existing fixture targets, plus the new GDI fixture target, were built
explicitly. All CTest invocations use `/tmp/raven-ctest-lock.sh`.

Commands used for the broad selection and complete suite:

```sh
/tmp/raven-ctest-lock.sh chromium-post-rebase-graphics \
  ctest --test-dir build-chromium -j4 \
  -R 'dxmt|dcomp|present|window|cocoa|fex' --output-on-failure
/tmp/raven-ctest-lock.sh chromium-final-suite env RAVEN_PREFIX_SETUP=1 \
  ctest --test-dir build-chromium -j4 --output-on-failure
```

The four persistent DWrite/DPI failures also reproduce on the unmodified parent
build; [baseline results](baseline-unrelated.json) record the binary hash,
source HEAD, exit statuses and output. The new GDI integration fixture fails
against that build: [baseline rejection](gdi-baseline.log).

Raw test logs, full configuration arguments and owned-process records are
retained locally in `/tmp/raven-chromium-investigation`.

An early concurrent application/test run breached the requested disk reserve:
the lowest sampled free space was approximately 6.5 GiB after a rapid drop.
Owned work was stopped and disposable output removed to recover above 8 GiB;
this constraint was not satisfied throughout the investigation. Subsequent
application runs were sequential. No debugger was attached, no push was made,
and shared components, fixtures and other worktrees were not edited.

## Final results

- Complete final suite with RAVEN_PREFIX_SETUP=1, -j4: **1,576 passed,
  4 failed, 8 skipped, 1 disabled** (1,588 runnable; 201 seconds).
- All **284** tests matching `dxmt|dcomp|present|window|cocoa|fex` that ran
  passed in that final suite; the two optional mtld3d FEX cases skipped.
- All three new regressions passed. The two GDI tests verify pixels and resize
  under parallel Rosetta and FEX; the offset unit test verifies mapping lifetime.
- The four persistent failures are `native_dwrite`, `native_x86_dwrite`
  (guest result 28), and `native_darwin_desktop_dpi_x64/x32` (guest result 11).
  Each reproduces with the unmodified parent binary. They were not weakened or
  changed in this branch.
- Serial selected recheck: 15/19 passed, with only those four baseline failures.
  Earlier transient capture, DComp, Mullion and DXMT device/fullscreen failures
  cleared. Earlier missing-fixture/relink failures are not final results.

[Machine-readable summary](results.json), [full final output](full-suite.log),
and [serial recheck output](regressions.log) preserve the results.
