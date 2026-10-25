# Native x86 API parity

The comparison starts at `fb2b97b`. It counts registered `ApiRegistry` handlers and guest component exports, including ordinal-only exports.
The x86 runner already registers the shared file, registry, system, shell, security, sockets and cryptography services. Its `with_imports`
adds **every public Windows export name**, just as x64 does. A name resolving in a provider is not evidence of an implementation.

`RAVEN_API_INVENTORY=/tmp/x86.tsv raven --run-process ...` records the actual registrations and initial graph before executing the program.
`scripts/native-api-inventory.py` compares two snapshots and optionally reads additional component directories directly from their PE export
tables. `--json` preserves the complete per-DLL name lists and differences. Counts measure reachability, not behavioral completeness:
component exports can contain ReactOS stubs, and the dedicated environment, memory, loader, desktop and CRT dispatchers are outside the
registry count. COM vtable methods and aliases are not additional named APIs.

## Inventory

The per-DLL x64 names absent from the x86 registry/component view decrease from **800 to 150** (including dedicated-dispatcher and architecture differences).

Counts are **host/guest**; `before → after` is shown only when changed. Missing counts are x64 names absent from the x86 registry/component view, before → after. Dedicated x86 dispatchers can supply some of those names (see below). DLL suffix `.dll` is omitted.

| DLL | x64 H/G | x86 H/G | Missing |
|---|---:|---:|---:|
| advapi32 | 187/0 | 187/0 | 0 |
| advpack | 0/83 | 0/83 | 0 |
| atl100 | 0/52 | 0/52 | 0 |
| atlthunk | 4/0 | 4/0 | 0 |
| bcrypt | 2/0 | 2/0 | 0 |
| bcryptprimitives | 1/0 | 1/0 | 0 |
| cabinet | 0/14 | 0/14 | 0 |
| cfgmgr32 | 6/0 | 6/0 | 0 |
| combase | 29/2 | 29/2 | 0 |
| comctl32 | 0/181 | 0/181 | 0 |
| comdlg32 | 6/4 | 6/4 | 0 |
| crypt32 | 28/0 | 28/0 | 0 |
| d2d1 | 0/12 | 0/0 → 0/12 | 12 → 0 |
| dbghelp | 2/0 | 2/0 | 0 |
| dsound | 12/0 | 12/0 | 0 |
| dwmapi | 10/0 | 10/0 | 0 |
| dwrite | 0/1 | 0/0 → 0/1 | 1 → 0 |
| gdi32 | 361/0 | 361/0 | 0 |
| gdiplus | 0/630 | 0/630 | 0 |
| hid | 1/0 | 1/0 | 0 |
| imagehlp | 3/0 | 3/0 | 0 |
| imm32 | 21/0 | 21/0 | 0 |
| iphlpapi | 7/0 | 7/0 | 0 |
| kernel32 | 601/23 → 604/23 | 599/0 → 605/0 | 28 → 25 |
| kernelbase | 601/22 → 604/22 | 0/0 → 605/0 | 623 → 24 |
| ktmw32 | 5/0 | 5/0 | 0 |
| libjpeg | 0/160 | 0/160 | 0 |
| libpng | 0/256 | 0/256 | 0 |
| libtiff | 0/16 | 0/16 | 0 |
| libxslt | 0/242 | 0/242 | 0 |
| loadperf | 6/0 | 6/0 | 0 |
| lz32 | 5/0 | 5/0 | 0 |
| mlang | 0/14 | 0/14 | 0 |
| mpr | 9/0 | 9/0 | 0 |
| msftedit | 0/9 | 0/9 | 0 |
| msi | 0/296 | 0/296 | 0 |
| msimg32 | 2/0 | 2/0 | 0 |
| msvcrt | 0/541 | 0/595 | 35 |
| msvfw32 | 0/47 | 0/47 | 0 |
| mswsock | 2/0 | 2/0 | 0 |
| msxml3 | 0/11 | 0/11 | 0 |
| ncrypt | 14/0 | 14/0 | 0 |
| netapi32 | 3/0 | 3/0 | 0 |
| ntdll | 90/8 | 93/6 → 93/8 | 15 → 13 |
| ole32 | 27/43 | 27/43 | 0 |
| oleacc | 1/0 | 1/0 | 0 |
| oleaut32 | 0/418 | 0/418 | 0 |
| pdh | 1/0 | 1/0 | 0 |
| powrprof | 1/0 | 1/0 | 0 |
| propsys | 0/208 | 0/208 | 0 |
| psapi | 13/0 | 13/0 | 0 |
| raven-rtl | 0/29 | 0/0 | 29 |
| raven-setupapi | 0/605 | 0/605 | 0 |
| raven-shell | 0/29 → 0/32 | 0/29 → 0/32 | 0 |
| raven-storage | 0/35 | 0/35 | 0 |
| raven-user32 | 0/100 → 0/101 | 0/100 → 0/101 | 0 |
| ravenhost | 18/0 | 11/0 → 18/0 | 7 → 0 |
| riched20 | 0/9 | 0/9 | 0 |
| rpcrt4 | 0/545 | 0/542 | 4 |
| runtimeobject | 16/0 | 16/0 | 0 |
| secur32 | 5/0 | 5/0 | 0 |
| sensapi | 2/0 | 2/0 | 0 |
| setupapi | 5/588 | 5/588 | 0 |
| sfc | 7/0 | 7/0 | 0 |
| shcore | 13/0 | 13/0 | 0 |
| shell32 | 35/7 → 37/10 | 35/7 → 37/10 | 0 |
| shfolder | 2/0 | 2/0 | 0 |
| shlwapi | 72/8 | 72/8 | 0 |
| sxs | 0/3 | 0/3 | 0 |
| ucrtbase | 0/541 | 0/610 | 20 |
| uianimation | 0/2 | 0/0 → 0/2 | 2 → 0 |
| uiautomationcore | 15/2 | 15/2 | 0 |
| urlmon | 0/110 | 0/110 | 0 |
| user32 | 295/99 → 295/100 | 295/99 → 295/100 | 0 |
| userenv | 4/0 | 4/0 | 0 |
| usp10 | 17/0 | 17/0 | 0 |
| uxtheme | 60/0 | 60/0 | 0 |
| version | 10/0 | 10/0 | 0 |
| wer | 2/0 | 2/0 | 0 |
| wevtapi | 4/0 | 4/0 | 0 |
| windowscodecs | 0/118 | 0/118 | 0 |
| winhttp | 8/0 | 8/0 | 0 |
| wininet | 1/0 | 1/0 | 0 |
| winmm | 52/0 | 28/0 → 52/0 | 24 → 0 |
| wintrust | 4/0 | 4/0 | 0 |
| ws2_32 | 57/0 | 57/0 | 0 |
| wsock32 | 31/0 | 31/0 | 0 |
| wtsapi32 | 4/0 | 4/0 | 0 |

## Changes and causes

- `SHBrowseForFolderW/A` was absent for both widths. Both now use the shell component's existing `FOS_PICKFOLDERS` dialog. The guest compiler
  supplies each width's `BROWSEINFO` and callback layouts. Initialization callbacks, ANSI/wide selection messages and cancellation work;
  the returned filesystem PIDL uses Raven's existing format and the task allocator. `SHGetPathFromIDListA` and `ILFree` join the shared
  wide converter and `CoTaskMemFree`. The picker is a filesystem browser; shell namespace objects, root confinement and the complete set
  of legacy `BIF_*` callbacks/options remain unimplemented for both widths.
- `GetEnvironmentStrings` was intercepted by `NativeX86Compat` and returned an empty block, while `FreeEnvironmentStringsA` did nothing.
  ANSI and wide enumeration now share the same variable store and allocator. WOW64 defaults now report `ProgramFiles`, `CommonProgramFiles`
  and processor architecture consistently with special-folder and registry APIs, retaining the native directories through `*W6432` names.
- x86 `GetVersionExA` returned a hard-coded version and omitted extended fields. It now reaches the same manifest-aware handler as x64.
- `InternetCrackUrlW` used incorrect hard-coded x64 offsets, also for x86, and the wrong HTTPS scheme number. Pointer stores, field offsets
  and the two-byte port now follow the caller's actual `URL_COMPONENTSW` layout.
- The controls provisioner explicitly disabled x86 DirectWrite, Direct2D and Windows Animation. It now builds the same sources at PE32
  addresses, with decorated stdcall imports of the host services. Both initial and dynamic x86 loaders resolve the Windows library names.
  The x86 runner registers the existing canvas and waveform services instead of its no-device waveform substitutes.
- Common controls, shell, automation, structured storage and setup components were already wired for x86 on the base. Reprovisioning is
  necessary: the supplied component directories predate several base source changes. No shared component directory was modified.

- Shared POINT-by-value and 64-bit scalar arguments consumed the wrong number of x86 stack words. Small adapters now reconstruct the
  original values before invoking the existing handlers, including window hit testing, byte-size formatting and trace registration.
  `ScriptStringCPtoX/XtoCP` had incorrect GDI argument counts; `RoResolveNamespace` and `GetThemeBackgroundRegion` counts were also corrected.
- KernelBase had no x86 provider, despite the shared implementations being present. It now mirrors Kernel32, including PathCch helpers.
  NTDLL's `strncpy` and `_vsnprintf` now forward to the same x86 CRT component.
- The destination edit contained the correct path even on the base. A later default parent `WM_PAINT` erased it; that path completed
  painting directly and bypassed `EndPaint`'s child invalidation. Both paths now share completion, and the path appears immediately.

## Remaining differences

The 25 Kernel32 / 24 KernelBase names include the five heap operations served by the dedicated x86 allocator, vectored exception handling
and raising/unwinding served by the x86 runtime, and x64 unwind-table/context interfaces. NTDLL's 13 remaining names are those x64 unwind
and function-table interfaces. The x64 `raven-rtl` component is not built for x86; x86 heap and synchronization operations use existing
native dispatchers rather than the shared guest RTL. This remains an architectural implementation difference.

The CRT differences comprise x64 C++ mangled names, x64 exception machinery, and runtime entry points already supplied by the dedicated
x86 CRT (for example `exit`, `_initterm`, `rand` and `srand`). RPC's four remaining exports are `Ndr64AsyncClientCall`, `NdrClientCall3`,
`NdrServerCallAll` and `NdrpClientCall2`, absent from the provisioned PE32 component. These are remaining component differences, not
silently redirected to incorrect calling conventions. No claim is made that every ReactOS export implements its complete Windows contract.

WinHTTP (8), WinINet (1), bcrypt (2) and secur32 (5) expose the same limited host implementations at both widths; unsupported APIs,
protocol options and upstream component stubs remain unsupported at both widths. D3D9, DirectSound, Vulkan and architecture-specific
exception/COM dispatchers have separate implementations and are not covered by the registry counts or these representative ABI fixtures.

The complete named/ordinal inventory can be reproduced with the committed script; this run's snapshots and full per-name JSON are in
`/tmp/parity-inventory/{before,after}{32,64}.tsv` and `/tmp/parity-inventory/{before,after}.json`. The before snapshots use `fb2b97b` with
only the inventory recorder added; its untouched executable was used for baseline tests. Counts include all supplied component DLLs,
including those loaded only on demand. Reprovisioning also incorporates base changes absent from the supplied binaries (`SHGetMalloc`
and one user component export), so not every component count increase originates in this patch series.

## Verification and Steam

Steam's original download was left untouched; the private copy has SHA-256
`7d3654531c32d941b8cae81c4137fc542172bfa9635f169cb392f245a0a12bcb`. Setup completed in
`/tmp/parity-steam/prefix/drive_c/Program Files (x86)/Steam`, creating `Steam.exe`, the uninstaller and desktop/Start Menu shortcuts.
The user drove installation. Welcome/header logos rendered; this is not an exhaustive image-resource audit.

Manual verification after the painting fix: the destination is prefilled immediately; Browse opens the shared filesystem dialog;
selecting Program Files (x86) returns successfully with NSIS's Steam suffix. Screenshots:
`/tmp/parity-steam/screenshots/03-baseline-empty.png`, `04-destination-fixed.png`, `05-folder-picker.png`, and `06-updater-error.png` (the user-supplied first-run failure).

The installed Steam updater starts but fails its first manifest download after about a minute with “Steam needs to be online to update.”
Its log names `https://cdn.steamstatic.com/client/steam_client_win32`; DNS and nonblocking connect succeed, a 517-byte send succeeds, but
no read readiness/receive follows in the trace. Host curl reaches the same URL with HTTP 200. The cause of the unanswered guest TCP/TLS
request is unresolved; this does not establish a problem with the user's network. The updated Steam client/login screen was not reached.
The bootstrap log is in the installed Steam `logs/bootstrap_log.txt`; the API trace is `/tmp/parity-steam/setup-2.log`.

Full serial comparison against `fb2b97b` completed (454 seconds each):

| Run | Total | Passed | Failed | Skipped |
|---|---:|---:|---:|---:|
| Baseline | 295 | 277 | 15 | 3 |
| Final | 303 | 289 | 11 | 3 |

**No new failures, removed tests or changed skips. All eight added tests pass.** Existing `native_dynamic_dialog_x64/x32` and
`native_bitmap_file_x64/x32` improve from failed to passed with the reprovisioned components.

The eleven remaining failures also fail on the baseline: `native_memory_contract`, `native_layout_failure_component`, `native_coordinate_contract`, `native_dwrite`, `native_secure_stream_x64`, `native_gamma_x64`, `native_desktop_icon_contract`, `native_string_resource_contract`, `native_dynamic_loader_contract`, `native_dll_lifecycle`, `darwin_dxmt_tessellation`.

The final command was `/tmp/raven-ctest-lock.sh parity ctest --test-dir build -j1 --timeout 120 --output-on-failure --output-junit /tmp/parity-final.xml`.
The baseline used the untouched cloned build with the same serial options. Logs and JUnit are `/tmp/parity-baseline-full.{log,xml}` and
`/tmp/parity-final.{log,xml}`; `/tmp/parity-suite-comparison.json` records the exact per-name comparison. An earlier partial final run was
stopped after another worktree started tests concurrently; it is excluded. The locked final run completed without that overlap.
The slot was released and `/tmp/raven-ctest.parity-done` created. No further tests or benchmarks are queued by this task.

The new fixtures call the same contracts in both widths: environment enumeration and WOW64 defaults, registry ProgramFilesDir, version
reporting, URL and proxy structure guards, sockets, data protection, BSTRs and actual common-control image lists; folder selection through
initialization callbacks, both PIDL converters, both free functions and cancellation; Direct2D geometry, animation doubles and canvas pixel
round trips. The existing DirectWrite interface fixture also runs as x86. Existing API-contract and version fixtures cover SetupDi device-set lifetime,
OLE task allocation and actual version-resource reads at x86.

The explicit shared x86 lists are checked against live registered handlers. This found a stale `GetCurrentProcessId` entry (served by the
dedicated environment dispatcher). An assertion over all provider exports would fail by design in **both** widths: Raven publishes
unimplemented Windows names to allow imports to bind and report unsupported calls when used.

## Reprovisioning

Rebuild `scripts/provision-controls.py` for **both x64 and x86**. x86 now includes the three graphics/animation components; x64 and x86 shell
DLLs need the new folder-browser exports. Rebuild installer components for both widths to pick up the **base** OLE DIB-section patch.
Validation uses `/tmp/parity-controls64`, `/tmp/parity-controls32`, `/tmp/parity-installer64` and `/tmp/parity-installer32` overlays.
The x64 overlay retains the supplied graphics/CRT DLLs; its shell and the x86 controls/graphics are rebuilt, as are both installer overlays.
The tool PATH needs `raven-components/tools/bin` for `widl` and Homebrew's `bison/bin` for the installer grammar.

## Upstream-facing diff

All implementation changes are platform-neutral. No Darwin implementation source changed. Line counts against `fb2b97b`:

| File | Added / removed |
|---|---:|
| `native/CMakeLists.txt` | +68 / −0 |
| `native/desktop/folder_browser_test.cpp` | +59 / −0 |
| `native/desktop/theme.cpp` | +1 / −1 |
| `native/desktop/windows.cpp` | +33 / −28 |
| `native/desktop/windows.h` | +1 / −0 |
| `native/main.cpp` | +2 / −2 |
| `native/nt/api.cpp` | +8 / −0 |
| `native/nt/api.h` | +2 / −0 |
| `native/nt/platform_api.cpp` | +10 / −7 |
| `native/nt/providers.cpp` | +24 / −0 |
| `native/nt/providers.h` | +3 / −1 |
| `native/nt/registry_words.inc` | +9 / −0 |
| `native/nt/shell_api.cpp` | +10 / −2 |
| `native/nt/system_api.cpp` | +38 / −24 |
| `native/nt/system_arguments.inc` | +2 / −2 |
| `native/nt/x86_compat_api.cpp` | +4 / −26 |
| `native/nt/x86_compat_api.h` | +1 / −1 |
| `native/nt/x86_loader.cpp` | +9 / −0 |
| `native/nt/x86_process.cpp` | +57 / −11 |
| `native/upstream/raven-shell/dialog.c` | +20 / −0 |
| `native/upstream/raven-shell/shell.c` | +65 / −0 |
| `native/upstream/raven-shell/shell.h` | +4 / −0 |
| `scripts/generate-registry-words.py` | +6 / −0 |
| `scripts/generate-system-exports.py` | +3 / −0 |
| `scripts/native-api-inventory-test.py` | +34 / −0 |
| `scripts/native-api-inventory.py` | +109 / −0 |
| `scripts/provision-controls.py` | +14 / −10 |
| `testdata/native_loader/folder_browser_client.c` | +95 / −0 |
| `testdata/native_loader/graphics_parity_client.c` | +43 / −0 |
| `testdata/native_loader/library_parity_client.c` | +152 / −0 |
