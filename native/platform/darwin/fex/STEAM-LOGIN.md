# Steam login investigation (2026-10-31)

Started at macos ac36a2f077f8c7b70faf85ae41bca6d30d4b04ef in a private
worktree/branch. No shared component or pristine prefix was changed.

## Upstream comparison

Final upstream refresh (`git fetch origin`) resolved origin/main to 3cd4ca60.
Its NativeMemory::free still unconditionally discards the fallback range,
including reserved/protected pages. GitHub issue/PR searches in
mirkobrombin/raven for "FEX YMM", "decommit" and "Steam" returned no matching
items during the final recheck. No upstream fix was imported.

## First blocker: Steam's SHA loop

With shared modules, Steam finishes updater verification and starts CEF, but
its main process faults at steamui.dll+0x3c98d (`movdqu (%r9), %xmm0`).
Repeated fault traces show R9=0x678bffff, R10=0x218827e9 and
R11=0x20000000000. The SHA prologue establishes R9 as its input pointer and
R11 as its stack anchor; the exception handler consequently faults too, at
steamui.dll+0x3f648 while reading R11-0x88. The crash handler terminates the
main process with status 100. CEF's subsequent shutdown is a consequence.

Patch 0028 conservatively disables multiblock compilation in the Raven x64
frontend. This removes the observed SHA fault and allows the main process to
stay alive. This is a correctness workaround with a performance cost, not a
completed repair of FEX's multiblock register reconstruction. The exact
optimizer/snapshot defect remains to be minimized. A new two-thread loop
fixture checks live pointer/stack/vector state across back edges on both
engines; it also passes the baseline and is therefore coverage, **not** a
standalone reproducer of the Steam SHA defect. Steam is the failing-before,
working-after workload for this workaround. The expanded fault diagnostics
retain the register evidence needed for further investigation.

Private module root: `/tmp/raven-steam-private-modules`.
Initial build: `scripts/build-fex-modules.py --root /tmp/raven-steam-private-modules
--raven-host --build-only --build-id steam-single-block-0028`.
The script completed module build/audit and native host preflight. Its module
hash and redirected RavenProcessInit RVA are pinned with the patch. Do not
use the old shared modules with this updated host contract.

## Second blocker: decommitting uncommitted/protected pages

After the SHA workaround, a renderer stops at libcef.dll+0x1901043.
The preceding operation is VirtualFree(address, size, MEM_DECOMMIT), and
GetLastError returns ERROR_NOACCESS (998). NativeMemory::free previously
asked FEX to zero the whole range through guest writes, which rejects
reserved, read-only and no-access pages. Windows allows those decommits,
including repeated decommit of already reserved pages.

The fix visits committed runs only, temporarily makes those runs writable,
discards their data, then makes the whole decommitted range inaccessible.
Uncommitted portions require no storage or writes. The regression fixture
covers all these states and checks MEM_RESERVE plus zero-filled recommit.
The baseline FEX executable returns fixture failure 2 (decommit of an entirely
reserved range); both engines pass after the fix.

## Third blocker: cooperative YMM snapshot loss

An anonymous cooperative launch could still crash a renderer at
libcef.dll+0x2ee51d4 (null dereference) or +0x3505d57, whereas parallel
scheduling reached the complete sign-in form. A separate two-thread AVX probe
proved a concrete snapshot defect: each thread keeps a distinct value in the
upper half of YMM0 across a busy loop with no guest ABI calls. The old FEX
snapshot returns failure 1; the updated snapshot returns 0. Caller-saved ABI
rules do not permit corruption during scheduler preemption.

The public legacy CONTEXT packet carried only the low vector halves. Patch
0029 exports explicit high-vector save/restore helpers; FexProcess includes
these 256 bytes in its internal 832-byte XSAVE-shaped snapshots. Legacy
512-byte context updates still preserve the upper halves. The public Windows
XSTATE API support is not expanded by this internal scheduler fix.
`native_ymm_snapshot_{native,fex}_{0,1}` covers both engines and scheduling
modes, with capability skip 77 when AVX is not advertised.

Final private build id: `steam-ymm-0029`; the same build command/root above
applies with this id. Native host preflight and module audit pass. Shared
components remain untouched. The renderer fault's attribution to this loss
is an inference; the register-loss regression is directly reproduced.

## Reproduction and evidence

Build is RelWithDebInfo, -j8, with explicit fixture targets. Component/font/PNG
cache entries came from the supplied build-macos cache. Skia source/library
and font directory were additionally required to enable CreateFontW; omitting
those paths produced an unrelated early updater failure.

Prefixes `/tmp/raven-steam-login-{fex,native}` were APFS clones of the supplied
pristine prefix. That prefix unexpectedly contained a remembered account:
the first Rosetta launch automatically restored it without input. It was
stopped, and config/userdata/Local Steam cache were moved aside in both
private copies for anonymous verification. A later FEX launch unexpectedly
restored the account again because HKCU\Software\Valve\Steam still contained
AutoLoginUser. It too was stopped; AutoLoginUser was set to an empty REG_SZ
with reg.exe in the private prefix, and the regenerated Steam/config was
moved aside. The final verification then showed empty account/password fields.
No credentials were entered, no sign-in action was taken, and no QR was scanned;
the automatic session restores were unintended.

Launch with RAVEN_PREFIX_SETUP unset, RAVEN_PARALLEL=1, the chosen private
prefix, and:

```
build-steam/native/raven --run-process --execution=fex --timeout-ms=360000 \
  --desktop=macos '--directory=C:\Program Files (x86)\Steam' \
  '/tmp/raven-steam-login-fex/drive_c/Program Files (x86)/Steam/steam.exe' \
  -- -cef-enable-debugging
```

Use RAVEN_TRACE_API, RAVEN_TRACE_MEMORY and RAVEN_TRACE_FAULT for diagnosis.
`RAVEN_DXMT_CAPTURE_DIRECTORY` reads actual IOSurface pixels into RGBA files,
independent of display lock. Convert width/height-named files to PNG.
Local logs/captures are under `/tmp/raven-steam-login-evidence` and are not
committed because the initial prefix/screenshots contain personal account
information. CEF `/json/list` gives independent page-title evidence.

Child process commands retain --execution=fex for GPU, renderer and utility
processes. Steam supplies service-sandbox-type flags; no sandbox-disabling
flags were added. Named-pipe/shared-memory/job initialization progresses to
CEF shared JavaScript first paint/load completion. No change to those
subsystems was needed for these blockers.

VirtualQuery still fails for some synthetic-DLL padding addresses (including
0x6fff00037000), and CEF still logs NOTREACHED checks and a Skia shader
compilation error. These diagnostics were not suppressed. Full VirtualQuery
MEM_MAPPED/guard/untracked-host-region parity remains outside this repair.
No permission prompts were clicked, no debugger attached, and nothing pushed.

## Qualification

Every CTest invocation used `/tmp/raven-ctest-lock.sh`; ordinary tests used
RAVEN_PREFIX_SETUP=1. Fixture targets were built explicitly with -j8.

- Unchanged-source full suite: 1,496 registered, 1,486 passed, 8 skipped,
  1 disabled, 1 failed (`native_darwin_cocoa_input`, posted key missed window).
- Earlier FEX-focused selection plus Cocoa recheck: 404 including cleanup,
  402 passed, 2 skipped; Cocoa input passed.
- Final patches-0029 full suite: 1,512 registered, **1,502 passed, 8 skipped,
  1 disabled, 1 failed**, 159 seconds. The sole failure is the same Cocoa
  input test, with the same posted-key/window diagnostic. No new failure
  appeared versus the same-source baseline.
- Final full suite includes **222 names containing fex: 220 pass, 2 optional
  mtld3d skips**. All eight new engine/scheduler regression lanes passed.
- The mixed decommit regression fails before the fix and passes both engines
  after it. The YMM probe returns failure 1 before the fix and zero after it.
- Final build/audit, explicit fixtures and git diff --check passed. Observed
  free disk remained above 8 GiB (lowest observed 17 GiB).

The full run is not itself green. The final serial Cocoa recheck passed (2/2
including prefix cleanup); see cocoa-final.log.
The supplied macos clean-except-load-sensitive history agrees with the sole
failure reproduced here, but was not substituted for the measured baseline.

## UI scope

Rosetta's anonymous launch renders the full sign-in form (empty username and
password fields plus QR code). **The final FEX build with patches 0028/0029
and RAVEN_PARALLEL=1 also renders that complete form**, via ANGLE_D3D11 on
Apple M4 Pro. Verified surface readback evidence:

- `/tmp/raven-steam-login-evidence/fex-login-final/700x440.png`
- `/tmp/raven-steam-login-evidence/native-anonymous/700x440.png`
- `/tmp/raven-steam-login-evidence/pages-login-final.json`: CEF page title
  "Sign in to Steam" and SharedJSContext.

Window/surface readback, rather than desktop screencapture, is the verification
method. The final Steam process and its own children were stopped at login.
No macOS permission prompt was interacted with.

With final patch 0029 and cooperative scheduling, the renderer survives and
the "Sign in to Steam" page/window renders, but the observed surface remains
"Waiting for network...". No claim of cooperative network/login parity or
of a usable FEX library view is made. A library view requires account use and
was not pursued. The unintended restores described above are not library
qualification.
