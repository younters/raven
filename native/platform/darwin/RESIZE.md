# Live resize

## Design

Cocoa keeps and scales the last GDI contents while the host window changes size. Host changes coalesce to the latest size at each
desktop poll. At most one placement generation is outstanding; further changes replace the pending size. The guest's placement,
including an adjustment or a veto, acknowledges the geometry; publishing a bitmap never authorizes the next placement. An older
acknowledgement cannot undo a newer host change, including a change from A to B and back to A.

The Windows placement protocol owns the order of `WM_WINDOWPOSCHANGING`, `WM_NCCALCSIZE`, `WM_WINDOWPOSCHANGED`
and the default `WM_SIZE`. Acknowledgements also retire requests dropped because their window was hidden, destroyed, reparented, no
longer resizable, already at the requested size, or given an invalid size. A failed placement acknowledges the remaining geometry
before it rethrows. A missing target reports a zero size, which retires the request without resizing a Cocoa window to zero.

A five-second deadline retires a lost delivery at the next Cocoa poll, after which a newer pending request can be delivered and late
completions are ignored. This recovers from a missing completion; it does not pace resizing, and it cannot help while the desktop
thread itself is blocked. A timed-out placement may still finish later; generation checks keep its acknowledgement from overriding
newer geometry.

Host geometry stays authoritative while a placement is outstanding. After completion, a repeated bitmap of the old size does not
roll the geometry back while the program catches up; frames of the new size go through normal presentation. Guest vetoes restore
the accepted geometry explicitly. Resizes that follow a screen change are queued for the program's message loop, as on Linux, and
do not take part in this protocol.

## Tests

`native_darwin_resize_state` covers the generation and acknowledgement rules and `native_darwin_resize_delivery` the delivery
through the desktop. `RAVEN_COCOA_RESIZE_TEST=1` makes Cocoa resize the first window from its right edge in steps, as a drag would, a few seconds
after it is shown.
