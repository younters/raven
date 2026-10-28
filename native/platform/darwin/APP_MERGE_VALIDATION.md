# Chrome and Steam merge validation (2026-10-27)

This run did **not** establish that Chrome or Steam works after the merge.
The worktree started at `5c7c304`. Fetching origin left `macos..origin/main` empty;
the repository PR/issue search found no targeted Chrome/Steam/WinINet fix.

## Installer fixes

Three independently observed blockers were corrected:

- MSI starts its custom-action server with `CreateProcessAsUserW(NULL, ...)`.
  Darwin now forwards that case through the existing process creator with the
  process's primary token. Non-null token validation is unchanged.
- On an arm64 host, WIDL's `-m32` generated ARM-style direct variadic arguments
  for x86 `NdrClientCall2`, whose implementation expects a pointer to the argument
  stack. Selecting `-b i686-w64-mingw32` generates that pointer. The provisioner
  now selects the existing compiler target triple at both widths. With the
  private rebuilt x86 components, Chrome's `ExtractTagInfoFromInstaller` completes
  instead of faulting in RPC marshalling at `0x1fc06510`.
- `SearchPathW` prepended search directories to MSI's absolute temporary EXE path.
  The Darwin helper preserves qualified paths. The embedded Chrome installer now
  starts instead of being silently skipped.

`native_darwin_process_creation_x64` and `_x32` exercise absolute SearchPath,
missing paths, invalid tokens, null-token suspended creation, resume, wait and
child exit status. Both fixtures returned zero in direct runs. Build used `-j4`.

## Deadline teardown

A later cleanup check found four copied Steam CEF hosts still running at about
100% CPU each after the launcher exited and their runtime deadlines elapsed.
`steam-orphan-sample.txt` places the host in `NativeThreads::thread_main` /
`schedule` repeatedly rebuilding Cocoa descriptor watches. Those exact owned
host/guest PIDs were stopped.

A minimal main-thread wait on an indefinitely sleeping worker reproduced a host
that survived a one-second deadline for at least 15 seconds. During teardown,
the main coroutine retained its waiting state. Finished worker coroutines then
had no ready caller to return to, while teardown disables normal deadlines.
The Darwin hook marks the teardown caller running before switching to workers,
so the existing switch marks it ready. The same x64 reproduction now exits with
the expected timeout in 1.12 seconds. The added teardown fixture runs at both
widths and has an external watchdog that cleans up only its own process family.
This addresses deadline teardown, not proof of Chrome's browser-host-kill path.

In `steam-teardown-fixed.json`, the retest tracked the complete owned process
family for 65 seconds: all CEF hosts had exited and only the launcher and its
guest remained, idle at 0% CPU. Those two recorded PIDs were stopped. The
launcher's missed deadline remains unresolved; this is not a claim that every
Steam shutdown path is fixed.

## Remaining Chrome blocker

The enterprise MSI lives at
`/Users/yount/Documents/Projects/raven-fixtures/downloads/googlechromestandaloneenterprise64.msi`.
The requested prefix is `/Users/yount/Documents/Projects/raven-fixtures/chrome`.
All MSI runs were headless (`/qn /norestart`, without `--desktop`).
An earlier failed prefix is retained as `chrome-before-rpc-fix`.

The embedded x86 installer subsequently faults at `0x64b3cf`, reading address
`0x1c`, in both default and parallel modes. Disassembly shows a null C++ object
receiver during the `system` switch lookup; this is a diagnosis of the immediate
fault, not its cause. MSI can report success despite the failed custom action.
There is no installed `chrome.exe`. Therefore sandboxed Chrome rendering,
`chrome://gpu`, WebGL FPS, actual Chrome orphan shutdown and Chrome Dock behavior
remain **unverified**. No sandbox bypass or executable patch was applied.

Private installer overlay reproduction (never rebuild shared components in place):

```sh
C=/Users/yount/Documents/Projects/raven-components
export PATH=$C/tools/bin:$(brew --prefix bison)/bin:$PATH
python3 scripts/provision-installer.py --arch x86 \
  --output /Users/yount/Documents/Projects/raven-fixtures/installer32-x86 \
  --archive "$C/installer32/reactos-source.tar.gz"
cmake -S . -B build \
  -DRAVEN_INSTALLER32_COMPONENT_DIR=/Users/yount/Documents/Projects/raven-fixtures/installer32-x86
cmake --build build -j4
```

## Steam and Notepad++

Steam was copied from `~/.local/share/raven/Steam` to `raven-fixtures/steam-test`.
The original prefix and its processes were not modified. The copied launcher
starts CEF browser, GPU and utility hosts without `RAVEN_CONTINUE_UNIMPLEMENTED`.
Set `--directory='C:\Program Files (x86)\Steam'` when reproducing.
The main browser window was not verified: the CEF browser repeatedly fails
`LoadLibraryExW` initialization of `steamclient64.dll`. A small launcher window
exists. An NSRunningApplication snapshot reported Regular policy for the launcher
and Accessory for the CEF hosts, but this does not prove the requested final UI.
No sign-in or terms acceptance was performed.

The WinINet trace produced no HTTP exchanges in these Steam launches. Diagnostics
show WinHTTP auto-proxy discovery and socket traffic; they do not establish an
`InternetOpenW` call. No speculative WinINet implementation was added.

Notepad++ portable was copied to `raven-fixtures/npp-test` with a separate
`npp-test-prefix`. It creates a 984x629 window without the continuation flag.
The copied updater exits with status `0xffffffff`: its libcurl startup calls
`InitSecurityInterfaceA`, which returns null with error 50. It does not reach a
WinINet request. Plugins Admin was not exercised. Keep the launcher's continuation
default: successful operation of all three applications has not been established.
No Firefox or DXMT/D3DKMT code was changed.

## Evidence and launcher bundle

Logs are in `/Users/yount/Documents/Projects/raven-fixtures/logs/chrome-steam`:
`install.log`, `install-retry.log`, `install-rpc-fixed.log`,
`install-path-fixed.log`, `install-exe-trace2.log`, `install-parallel.log`,
`creation64.log`, `creation32.log`, `steam-directory.log`, `steam-api.log`,
`npp-strict.log`, `gup-strict.log`, and `provision32.log`.
The large API traces contain local paths and request arguments.

`screencapture -x -o -l <windowid>` was attempted for Steam and Notepad++ but returned
`could not create image from window`. No screenshot or GPU evidence is claimed.
The saved `updater-disassembly.exe` is a truncated diagnostic copy with its PE
section lengths adjusted solely for disassembly; do not execute or distribute it.

The installed Steam bundle lacks `NSMicrophoneUsageDescription`. A replacement
was generated and its microphone string verified only in the test directory:

```sh
RAVEN_APPS_DIR=/Users/yount/Documents/Projects/raven-fixtures/steam-test-apps \
RAVEN_BINARY="$PWD/build/native/raven" \
  native/platform/darwin/raven-apps sync \
  /Users/yount/Documents/Projects/raven-fixtures/steam-test
```

`~/Applications/Raven` was untouched. The test bundle points at the copied prefix.

## Change scope

Implementation commits: `235ff04` (MSI launch hooks), `1425f6b` (WIDL target),
and `787ed41` (deadline teardown).

Upstream-file deltas from the starting commit:

- `native/nt/child_api.cpp`: +7 / -1.
- `native/nt/file_api.cpp`: +6 / -0.
- `native/nt/threads.cpp`: +6 / -0.
- `scripts/provision-installer.py`: +1 / -1.

Other implementation, tests and documentation are under `native/platform/darwin/`.
No push was performed.

## Area checks

All ctest invocations used `/tmp/raven-ctest-lock.sh` and `ctest -j1`.
The parent/other agents occupied the slot for most of the interactive work.
No full suite was run from this worktree.

- `network-orphan-tests.log`: 12/12 passed (x64/x86 library parity, WinHTTP,
  and the eight existing orphan contracts).
- `token-tests.log`: 4/4 passed (both existing process-token contracts and
  both new process-creation contracts).
- The first teardown ctest run passed x64; x86 reached the deadlock detector
  instead of the expected deadline because both guest waits were infinite.
  The worker now sleeps 60 seconds so the one-second deadline is the tested
  trigger. Both revised watchdogs pass directly (`teardown64-direct-final.log` and
  `teardown32-direct-final.log`). The locked final rerun remained queued behind
  other agents and was cancelled before starting; `teardown-tests-final.log`
  is empty. Rerun with:

  ```sh
  /tmp/raven-ctest-lock.sh chrome-steam-teardown ctest --test-dir build -j1 \
    --output-on-failure -R native_darwin_teardown_wait
  ```

These small orphan contracts do not substitute for the unperformed Chrome
browser-host-kill test. No `chrome://gpu` hardware claims are made.
