# FEX in the default macOS build

FEX is built by default on Apple silicon. Rosetta remains the default execution
engine. `raven --run-process --execution=fex ...` selects FEX for that invocation.
`-DRAVEN_FEX_MODHOST=OFF` removes the backend and companion. Provisioning the PE
modules is separate from configuring/building Raven: no llvm-mingw download or
module build occurs during the default build. The older opt-in statements in
M1D/M2/M3 describe those milestones, rather than the current build default.

Ship `raven-fex-host` beside `raven`. The normal build (including the `raven`
target) builds this companion and the registered FEX test fixtures. It shares
Raven's static runtime libraries and CLI/Cocoa sources; it is an arm64 process
hosting the existing xtajit64 module, not a new guest agent or an x86 launch.
Explicit FEX selection execs the companion before Cocoa startup, preserving PID,
arguments, descriptors and environment. A compile definition prevents re-exec.
There is no change to default/cooperative versus optional parallel scheduling.

Module search order is a nonempty `RAVEN_FEX_MODULE_ROOT` environment override,
the compiled configured `RAVEN_FEX_MODULE_ROOT`, then `fex-modules` beside the
resolved executable and `../raven-components/fex-modules`. Configure discovers
modules in a sibling `raven-components/fex-modules` directory when available.
An invalid explicit override fails instead of silently loading another module.
Absent modules produce a provisioning/search-path error at runtime. Only the
x64 `xtajit64.dll` is required here; WoW64 remains unsupported.

## Why a companion executable

XNU's `machine_task_process_signature` sets `task->preserve_x18` based on the
**main executable signature's SDK**, with a macOS SDK-before-13 compatibility
case (or private entitlements). This is a process/task decision, not an image
address-range property, and lowering deployment minimum alone is insufficient.
Source: <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/machine_task.c>.
A legacy-linked dylib therefore cannot preserve FEX's x18 under a modern main.

A measured dlopen probe confirms this on this host: compile the existing
`modhost/x18_probe.c` as a dylib with `-Dmain=x18_probe -dynamiclib -O2
-ffixed-x18 -Wl,-platform_version,macos,11.0,11.0`; dlopen it and call
`dlsym(library, "x18_probe")(1, argv)` from a tiny C main. Link that identical
main normally, then with `-Wl,-platform_version,macos,11.0,11.0`.
[default-x18-evidence.json](default-x18-evidence.json) retains results: modern
main lost x18 on 100/100 sleeps and 28/28 signals; legacy main lost none.
No entitlement, system setting or debugger was used.

The legacy option is now private to the companion and FEX-only test executables.
It no longer propagates from raven_cpu to every consumer. `otool -l` reports
min/SDK 27.2/27.2 for both the reference OFF Raven and this default Raven, and
11.0/11.0 only for the companion. Rosetta/Cocoa thus retains its original linked
SDK and minimum; FEX runs accept legacy framework behavior in their separate
executable. This avoids claiming that AppKit behaves identically across SDKs.
The default-discovery integration contract checks these separate load commands,
a missing-module override diagnostic and real process execution without the env.

The default-build qualification also found a pre-existing integration gap with
the current provider's generated NT syscall services: FEX still rejected
`enter_nt_service`. Its Darwin adapter now validates the pending syscall number,
post-SYSCALL RIP, executable SYSCALL/RET bytes and stopped call state, restores
RCX from R10, and enters the existing API result path. Pending syscall identity
is part of the selected thread state, so scheduler snapshots preserve it.
The engine contract checks wrong RIP/number rejection and argument/result flow.
This does not implement native Unix libraries, WoW64 or other unsupported APIs.

## Qualification on this branch

Fresh Release build: `/tmp/raven-default-fex`, copied RAVEN/cache/font/PNG settings
from the reference `build-macos/CMakeCache.txt`, omitting its explicit FEX=OFF
cache entry to exercise the new default. Configure and normal `-j8` builds pass.
An additional configuration with `/nonexistent/raven-fex-modules` succeeds and
builds both standalone host targets with `-j8`, without provisioning modules.

Registration: OFF **483**, default **606** (**+123**). Final FEX-only run:
**114/114 pass, no skips**, including the existing Tier 0 expected-failure test.
Every CTest command, including inventories and rechecks, used
`/tmp/raven-ctest-lock.sh`. The broad run was split after test 69 (the next test
was interrupted and rerun); the remainder used four concurrent tests. The NT
adapter fix was rebuilt during that run, then the entire FEX label rerun serially.

Across broad coverage and explicit rechecks: **595 pass, 6 skip, 1 disabled,
4 unresolved failures**. Cocoa policy/input/hidden surfaces, GDI presentation,
re-exec, Darwin contracts, DXMT window/presentation/resize/geometry and the
kernel ANSI/file/thread/SEH fixtures pass. DXMT identity/self-share, x86 pipe,
wait-guards, memory-small and orphan checks pass serially. Four x87 tests pass
after explicitly building their pre-existing optional fixture target.

This is **not a green full-suite qualification**. Remaining failures:
`native_layout_failure_component` returns 10; `native_cmd` times out;
`native_cmd_x32` returns 11 on recheck; `native_parallel_lifecycle_x64` faults at
`parallel-lifecycle64.exe+1025`. That exact lifecycle fault also reproduces with
the reference OFF executable against the same fixture and script. No baseline
claim is made for the other three failures. Initial parallel-run failures and
all recheck counts remain in [default-build-evidence.json](default-build-evidence.json).
Raw build/test logs are `/tmp/raven-default-fex-*.log`.

Notepad++ was copied from `raven-fixtures/npp` into
`/tmp/raven-default-fex-apps/npp`, with private prefixes and 20-second runtime
deadlines. The first Rosetta desktop capture did not show an editor. A retry
with the warmed prefix and Cocoa diagnostics produced the editor window,
confirmed by the window-info tool and a window-specific screencapture at about
12 seconds: `/tmp/raven-default-fex-apps/native-recheck-window.png`.
The reference executable also creates its editor window. Both sessions end at
the requested timeout; no editing or application-parity claim is implied.

Without any module environment override, FEX Notepad++ resolves 127 imports,
attaches PE DllMain, completes RavenProcessInit and ThreadInit, then rejects a
guest image address conflict at **0x140000000**. It creates no editor window.
Logs, commands and screen captures are under `/tmp/raven-default-fex-apps/`.
The companion design leaves FEX's inherited real-app/Unix-library/16K-page
limitations in place; enabling it at build time does not change the default engine.
