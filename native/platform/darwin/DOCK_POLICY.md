# Darwin Dock policy design note

A Windows process can connect to Cocoa without becoming a Dock application. A desktop connection starts as Accessory; the first
taskbar-eligible guest window promotes it to Regular before that window is ordered. Removing or hiding the last eligible window
schedules one reconciliation after the presentation/pump turn. A replacement arriving in that turn cancels demotion. There is no
timer-based hysteresis.

## Taskbar eligibility

The rule follows the Windows taskbar contract reviewed for this port:

- The window is effectively visible, top-level, not WS_CHILD, and not message-only.
- WS_EX_TOOLWINDOW excludes it unless WS_EX_APPWINDOW is also set.
- A Windows owner (GW_OWNER) excludes it regardless of visibility, unless WS_EX_APPWINDOW is set. SetParent hosting anchors do not
  count as owners.

Screen position and rectangle size do not affect eligibility. In particular, minimized placement outside the screen retains its
button. Menus and tooltips are excluded by their styles/ownership. Foreign GPU frames do not create guest top-level windows and
cannot create a Dock application on their own.

The generic desktop interface passes the window collection through one callback. Darwin gathers the facts and owns the rule. A
separate visibility-intent callback lets Cocoa see a ShowWindow request before the original upstream visible/style assignments;
those assignments retain their original order. Metadata collection, eligibility and policy transitions remain in the Darwin tree.

The Regular application menu, Dock reopen handling, minimize/restore behavior, WS_MINIMIZE deminiaturization guard, and
AppleActionOnDoubleClick volatile domain remain in effect.

## Application identity and process responsibility

A launcher identity requires an org.raven.* bundle identifier and nonempty RAVEN_APP_NAME/RAVEN_APP_ICON metadata. Those values
survive Windows child-host spawns, including when a windowless launcher starts the host owning the application's main window.
Generated launcher bundles do not set LSUIElement and remain pinnable.

A direct desktop host strips foreign bundle metadata before AppKit access. When its responsible ancestor is foreign, it resolves
responsibility_spawnattrs_setdisclaim dynamically and performs one same-PID posix_spawn exec with that attribute. Its descendants
retain Raven responsibility; Windows child-host spawns do not disclaim again. The exec uses the exact path from
_NSGetExecutablePath and preserves ordinary inherited descriptors. It temporarily clears CLOEXEC on explicit command-line fd
arguments, restoring their flags and clearing the handoff marker if exec fails. It does not add a supervisor or change exit
status.

An embedded runtime property list supplies org.raven.runtime identity. After the responsibility handoff, an optional
_RegisterApplication check-in supplies the runtime identity and starts the record as UIElement. This is necessary because
LaunchServices can otherwise retain the original ancestor's registration even after responsibility changes. This registration flag
belongs to the direct runtime record, not to generated bundles. Cocoa subsequently promotes eligible hosts to Regular. If these
private APIs are unavailable, Raven continues with public AppKit policy and process-name/icon updates; independent LaunchServices
attribution is not guaranteed in that fallback.

Only promotion of an unbranded host reads the executable's PE resources. The name uses FileDescription, ProductName, then the
executable stem. The first icon group supplies the best available image at the requested size; both PNG and legacy DIB entries are
supported. The executable is read in one bulk operation and its identity cached. Version-resource traversal is bounded to 32
levels and an 8192-node budget; each child is restricted to its parent span. The icon is applied after promotion because AppKit
can replace an icon while changing policy. An optional LaunchServices display-name update accompanies NSProcessInfo.processName.
Launcher metadata always takes precedence. NSRunningApplication.icon can report a bundle-path icon; the actual Dock tile is
checked externally as well.

## Foreground launch right

The ticket models only being started by the foreground process. It does not implement Windows' foreground lock, last-input rule,
or cross-process AllowSetForegroundWindow semantics; Raven's existing Windows API stub remains separate.

The user-launched host records the current foreground process and the launch-root PID in a shared one-use ticket. Child-spawn
metadata explicitly transfers an unconsumed ticket and identifies a child, preventing background helpers from minting new rights.
Only activation of a Dock-eligible window can spend the ticket; tool splashes and host focus synchronization cannot. The first
eligible host consumes the ticket under a file lock and activates after ordering its first window. The launch origin, launch root,
or current host must still be foreground. A workspace foreground-change notification permanently expires the ticket when another
application takes over, so returning to the launch origin does not revive it.

A denied activation leaves the ticket unconsumed, leaves focus alone and requests informational user attention, approximating a
Windows taskbar flash. Workspace expiry is recorded separately from consumption. Guest activate, SetFocus and SetForegroundWindow
requests reach the same Cocoa gate; an already foreground host can focus its later dialogs. Later helpers cannot reuse a consumed
ticket.

A shared lease descriptor keeps an unused ticket alive across the host family. It is CLOEXEC for Rosetta guests, explicitly
inherited by Windows child-host spawns, and closed on host exit. The final host removes both the ticket and lease files, including
when no window ever appeared. This avoids removing a ticket while a child is still starting.

## Verification

Darwin contracts cover owned and hidden-owner windows, APPWINDOW overrides, TOOLWINDOW, WS_CHILD, message-only windows, off-screen
and empty rectangles, and actual minimized placement. The Cocoa contract exercises delayed presentation, tool-splash/main-window
ordering, popup-to-top-level replacement, hosting anchors, standalone dialogs, minimize, tray hide/restore, replacement in one
batch, owned windows and the visibility-intent callback without modifying the upstream window record. Foreground tests cover one-
use and permanent expiry, and skip clearly without a GUI session.

The child-metadata contract checks replacement of transient environment entries, retention of launcher/DPI metadata and
withholding a consumed ticket. The same-PID exec contract asserts actual self responsibility in the native fixture and from the
live Python parent, even with an incorrect argv[0]. It also checks stdin/stdout/stderr, CLOEXEC inherit/channel/exit-code
descriptors and the exit status, plus no-window ticket cleanup. Existing child, handle-channel and prefix-setup contracts
supplement those checks. GUI-dependent contracts return skip status 77 when no GUI session or WindowServer is available;
responsibility contracts also skip if the required SPI is absent.
