# Window-driven Dock policy (implementation report)

This change is incomplete: direct CLI hosts still inherit the responsible application's fixed LaunchServices bundle ID on this macOS host. Their display names now update correctly through optional LaunchServices SPI, but full identity acceptance remains incomplete.

## Implemented

Desktop hosts connect as Accessory. Presentation carries effective guest visibility, style/ex-style, owner and guest/screen rectangles. Cocoa promotes before ordering an eligible frame, keeps minimized windows eligible and defers last-window demotion until the next Cocoa presentation/pump reconciliation. A replacement arriving before reconciliation cancels demotion; no timer is used. Existing application menu, reopen handling, WS_MINIMIZE guard and AppleActionOnDoubleClick domain are retained.

Foreign `__CFBundleIdentifier` metadata is removed in main before AppKit access. `org.raven.*` plus launcher name/icon is retained across child hosts. Generated bundles remain Dock-pinnable and do not gain LSUIElement.

A shared temporary foreground ticket records the launching foreground PID. Children explicitly inherit an unconsumed ticket and a child marker, preventing later helpers from minting new rights. The first qualifying host atomically consumes it under flock and activates only if its launch origin is still foreground (or it is already active). This is a macOS gate, not Windows AllowSetForegroundWindow lock semantics.

At promotion, an unbranded host reads its own PE resources: FileDescription, then ProductName, then executable stem; first icon group at best available 256px size, PNG or DIB decoding. Launcher metadata takes precedence. The resource code updates NSProcessInfo and NSApp's icon. An optional dynamically resolved LaunchServices display-name call (also used by WebKit) updates the externally reported name after check-in. The inherited bundle ID remains T3. This is a private API dependency with a fallback to the public process name/icon updates.

## Scripted evidence

All fixtures and prefixes were private copies under `/tmp/raven-dock-e2e`; bundles and logs also stayed there. Only recorded PIDs started for these runs were terminated. No debugger was used.

Counts below are sampled **Regular host records**, using both NSWorkspace/NSRunningApplication and lsappinfo, not a pixel count of Dock tiles. No pre-change baseline was captured.

- Direct Notepad++: 0 Regular before a window, 1 afterward; first qualifying window activated. Initial runs retained T3 name/bundle. Final run reports Notepad++ and activates; bundle remains com.t3tools.t3code. Icon presence was checked, but the external icon pixels were not compared.
- Fresh prefix followed by copied Notepad++: 0 during initial setup, maximum 1 once Notepad++ appeared. No extra Regular regsvr32 records observed.
- Firefox setup stub, copied installer/prefix: 0 initially, maximum 2 Regular hosts while installer windows were present. Their external identities were T3: FAIL for identity.
- Direct Firefox: 0 initially, 1 while its window appeared, 0 after exit. Strict direct launch stopped on WSCEnumProtocols. A retry with RAVEN_CONTINUE_UNIMPLEMENTED=1 reports Firefox and activates; maximum two Regular records appeared late in that run. This is not browser stability evidence.
- Firefox through a raven-apps shortcut bundle: one Regular host and multiple Accessory/UIElement helpers. Name Firefox, org.raven.app.firefox-prefix.7309cc8f6f3d, icon present on parent and child records. CGWindowList identified the visible Mozilla Firefox window as launch host PID 52707; this run did not establish a child-owned browser window. Another run hit a guest xul breakpoint; no OOM debugging was attempted.
- Cleaning environment, self-exec and optional responsibility-disclaim spawn experiments still left T3 attribution. Those unsuccessful workarounds were removed.

Raw ephemeral diagnostics: `/tmp/raven-dock-e2e/*results.jsonl`, `logs/Firefox.log`, and `*-build*.log` files in `/tmp`.

## Remaining work and risks

- Resolve the fixed direct LaunchServices bundle attribution and compare external Notepad++/Firefox icon pixels. Display names pass via optional SPI; the fixed bundle IDs still identify T3.
- Verify child-owned browser/dialog foreground transfer and later helper focus behavior with controlled foreground changes.
- Add/run real Cocoa transition coverage for delayed splash, dialog-only child, hide/show/tray, minimize and same-batch replacement. Current added contracts cover eligibility, ticket one-use/current-origin checks and environment filtering.
- The ticket checks the foreground PID at consumption; it does not detect leaving that app and returning to it before the first window. An unused ticket can leave a small temporary file.
- Owner visibility is supplied on presentation updates; owner removal without a child redraw needs stronger reconciliation coverage.
- Headless behavior follows the existing absence of a Cocoa backend; a dedicated per-PID headless E2E sample remains to be recorded.

## Validation

Build: supplied CMake configuration and full `cmake --build build -j4` succeeded.
Darwin/Cocoa and full suites were requested through `/tmp/raven-ctest-lock.sh`, `ctest -j1 --timeout 120`; final results are pending the shared lock.

Commits: 3730e5b (metadata), 12e329a (policy/identity/foreground), 0527dee (exe resources), 781222a (first-show visibility), 397e78f (contracts), cc40ffd (external display names).
