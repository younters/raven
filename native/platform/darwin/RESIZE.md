# macOS resize transactions

## Design and invariants

Cocoa retains and scales the last GDI contents while host geometry changes.
Host changes coalesce to the latest size at each desktop poll. At most one
placement generation is outstanding; further changes replace the pending size.
Guest placement, including adjustment or veto, acknowledges geometry. Bitmap
publication never authorizes the next placement. An older acknowledgement cannot
undo a newer host change, including A→B→A.

The existing Windows placement protocol owns WM_WINDOWPOSCHANGING,
WM_NCCALCSIZE, WM_WINDOWPOSCHANGED and default WM_SIZE ordering. Acknowledgements
also retire events discarded because their window was hidden, destroyed,
reparented, no longer resizable, already the requested size, or given an invalid
extent. A failed placement acknowledges the remaining geometry before rethrowing.
A missing target reports zero extent, which retires the transaction without
resizing a Cocoa window to zero.

A five-second steady-clock deadline retires a lost delivery at the next Cocoa
poll. Any newer pending request can then be delivered; late completions are
ignored. This is recovery from missing completion, not a pacing timer. It cannot
make progress while the desktop thread itself is blocked. A timed-out guest
placement may still finish later; generation checks prevent its acknowledgement
from overriding newer geometry.

Host geometry stays authoritative while placement is outstanding. After
completion, a repeated old-sized bitmap must not roll geometry back while the
producer is catching up. Actual new-sized frames continue through normal
presentation. Guest vetoes restore accepted geometry explicitly.

Upstream 9a091d54 queues screen-change resize events for the message loop rather
than invoking guest procedures during scheduler polling. That path is preserved;
its generation is zero and does not acknowledge a Cocoa transaction. The merged
fex-portal base also retains fix/maximize-stalls' private non-executable x64 data
write optimization, including protection/shared/image/WoW64 exclusions. Resize
changes do not alter that policy or attempt to solve maximize stalls.

## Measured results and limits

The earlier A/B experiment used five Chrome maximize/restore cycles per cell and
one two-second scripted host-size sweep (81 requested sizes). Source results are
in `raven-wip/resize/ab/RESULTS.txt` and `ANALYSIS.txt`.

| Producer route | Baseline final drag tail | Resize design final drag tail | Baseline maximize median | Resize maximize median |
|---|---:|---:|---:|---:|
| GPU / DXMT | 7220.730 ms | 353.686 ms | 515.649 ms | 516.151 ms |
| Software / foreign GDI | 2958.565 ms | 190.265 ms | 416.527 ms | 429.373 ms |

These are diagnostic observations, not production benchmarks. The old probe
scanned a control file on every Cocoa poll and materially perturbed scheduling.
A replacement probe must limit control reads to ten per second. The measurements
support reduced queued placement work, not a maximize speedup. Root GDI frames
may contain copied pixels. Producer-backed layer submission is neither scanout
nor proof that every page pixel was repainted. No GPU variant submitted DXMT
content during the sweep. These were scripted host changes, not real pointer
drags or validation of modal ENTERSIZEMOVE/EXITSIZEMOVE behavior.

## Hosted Metal limitation

The retained GDI layer scales, but separate hosted Metal views still use guest
geometry. They can leave uncovered regions or lag behind the host during resize.
Changing their autoresizing mask alone is insufficient: child positions, clipping,
DComp transforms and producer-owned drawable dimensions must remain consistent.
A follow-up needs a host visual container distinct from guest drawable allocation,
a mapping from accepted guest client bounds to current host bounds for the entire
subtree, and tests for child offsets, clipping, nested composition, DPI changes,
resize/veto/restore, and DXMT/DComp/Vulkan presentation. No drawable allocation or
foreign-producer protocol is changed here.

## Human qualification

Run Chrome with both normal GPU rendering and `--disable-gpu`. On a page with
text, scrolling and animation, drag each edge and corner continuously, then reverse
direction quickly and release. Verify the host follows the pointer, the final
client size settles, and input still reaches the window. Repeat after minimize /
restore, maximize / restore, moving between displays, and hiding / showing the
window. Check fixed-size dialogs reject resizing without becoming stuck. Watch
for stretched retained content, black uncovered Metal regions, stale final text,
and unexpected geometry rollback. Confirm normal title-bar maximize behavior
separately. These checks require a human with macOS input permission.

## Verification on the merged base

Implementation `21768e96` combines the resize branch with fex-portal `d7c8a73e`.
The full two-job CTest run completed 1,739 entries in 866.40 seconds: 1,709 passed,
7 skipped and 23 failed/not run. Seven failures cleared on targeted rechecks:
one intermittent process-list enumeration check and six tests whose fixture
executables are not included in the default build. The remaining 16 match the
supplied baseline failures or the known long-build-path bitmap failure. Resize
state/delivery, Cocoa policy/input/hidden-surface, native/FEX foreign GDI and the
FEX process-runner contract passed. No tests were weakened.

Chrome used fresh profiles, `about:blank`, FEX parallel execution, and three
maximize/restore cycles per rendering route. Each route also received one
81-step host-size sweep over two seconds; all 81 steps were consumed and 58 guest
placements dispatched. A single control-file command schedules the sweep on
normal Cocoa polls; control-file reads are limited to ten per second. This
preserves the original nominal 25 ms geometry cadence without hot file polling.

| Producer route | Final drag tail (one sweep) | Maximize median / maximum (3 samples) | Restore median / maximum (3 samples) |
|---|---:|---:|---:|
| GPU / DXMT | 344.756 ms | 349.177 / 1376.622 ms | 290.264 / 336.699 ms |
| Software / foreign GDI | 148.482 ms | 668.688 / 1275.387 ms | 628.216 / 1212.221 ms |

All twelve maximize/restore transitions had matching producer submissions; none
were censored. Drag tails are consistent with the earlier resize improvement.
These small, ordered, trace-enabled samples on a different base and a corrected
probe are not a paired speedup measurement. Maximize remains variable; no
maximize improvement is attributed to this resize change. The endpoints are
producer-backed layer submissions, not screen scanout or complete page repaint.
Real pointer dragging and visible intermediate Metal continuity remain untested.
Raw traces, exact binary hashes, instrumentation diff, test logs and analysis
are retained in `raven-wip/resize/round3/`.
