# Dock policy

A Windows process can connect to Cocoa without becoming a Dock application. A desktop connection starts as Accessory; the first
taskbar-eligible guest window promotes it to Regular before that window is ordered. Removing or hiding the last eligible window
schedules one reconciliation after the presentation/pump turn. A replacement arriving in that turn cancels demotion. There is no
timer-based hysteresis.

## Taskbar eligibility

Raven uses Windows taskbar eligibility to decide which guest windows get a Dock presence.

- The window is effectively visible, top-level, not WS_CHILD, and not message-only.
- WS_EX_TOOLWINDOW excludes it unless WS_EX_APPWINDOW is also set.
- A Windows owner (GW_OWNER) excludes it regardless of visibility, unless WS_EX_APPWINDOW is set. SetParent hosting anchors do not
  count as owners.

Screen position and rectangle size do not affect eligibility. In particular, minimized placement outside the screen retains its
button. Menus and tooltips are excluded by their styles/ownership. Foreign GPU frames do not create guest top-level windows and
cannot create a Dock application on their own.

The generic desktop interface passes the window collection through one callback. Darwin gathers the facts and owns the rule. A
separate visibility-intent callback lets Cocoa see a ShowWindow request before the shared code's visible/style assignments;
those assignments keep their order. Metadata collection, eligibility and policy transitions stay in `native/platform/darwin`.

A Regular application also gets the application menu, Dock reopen handling, minimize and restore, a guard against
deminiaturizing a `WS_MINIMIZE` window, and the user's title-bar double-click action (`AppleActionOnDoubleClick`).

## Application identity

A launcher provides an `org.raven.*` bundle identifier, name and icon. Windows child-host launches inherit this metadata.
For a direct desktop launch, Raven derives the name from PE version resources or the executable's filename, and the icon
from its PE icon resources. It applies the icon after changing the AppKit activation policy.

Raven looks up optional private macOS symbols at runtime:

- `_LSGetCurrentApplicationASN`, `_LSSetApplicationInformationItem` and `_kLSDisplayNameKey` update the LaunchServices display name.
- `responsibility_get_pid_responsible_for_pid` and `responsibility_spawnattrs_setdisclaim` support re-executing a direct launch
  in the same PID with independent process responsibility.
- `_RegisterApplication` supplies an independent LaunchServices application record for the runtime after that handoff.

Raven skips the corresponding operation when the needed symbols are absent. Public AppKit activation policy and process
name and icon updates still apply, but macOS may attribute the process to its launcher.

## Foreground launch right

The ticket models only being started by the foreground process. It does not implement Windows' foreground lock, last-input rule,
or cross-process AllowSetForegroundWindow semantics; Raven's `AllowSetForegroundWindow` is separate and does not touch the ticket.

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

## Tests

`native_darwin_dock_policy` checks taskbar eligibility, `native_darwin_cocoa_policy` checks activation policy across
window transitions, and `native_darwin_foreground` checks foreground ticket consumption and expiry.
