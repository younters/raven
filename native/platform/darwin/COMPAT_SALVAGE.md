# Compatibility salvage, 2026-10-29

Branch: `t3/salvage-compatibility-work`, based on `a401264` (`macos`).
Source branch remains at `08c7a9505c60a60535b533e3392d2e7d539114b9`.
No push, Chrome/Steam installation, application launch, or source-branch modification.
`7410180` is excluded; DXMT presentation belongs to another delegate.

## Decisions

| Source | Decision | Reason | Implementation |
| --- | --- | --- | --- |
| `235ff04` | keep | Null-token MSI custom-action launch and qualified SearchPath lookup remain missing. Preserve current token validation/propagation. | `815cff8` |
| `1425f6b` | keep | WIDL width alone selects the wrong calling convention on arm64. Select the guest target triple. | `8dad984`, regression `0e564b1` |
| `787ed41` | keep | Main coroutine can remain waiting during deadline teardown. Preserve newer channel wake integration. | `c9f61f2` |
| `63576dd` | keep | Useful historical blocker evidence, explicitly labelled as historical; no current app qualification inferred. | `afa7720`, this report |
| `a66e44d` | keep | An unanswered TCC prompt must not block the guest scheduler; consolidate duplicate embedded plist sections. | `cdfc70b` |
| `d7ac645` | keep | Missing socket option; keep exclusivity state with each socket and retain current socket catalog. | `ee3892a`, tests `f820ce0`, `f266a57` |
| `3294bec` | keep | ANSI SSPI table remains stubbed; reuse existing W credential/context engine with ANSI adapters. | `1c7db72` |
| `9887172` | keep, reapply | PR #27 supplies the loader machinery but its default allowlist excludes this compiler. Replace eager preload with a Darwin admission seam using that machinery. | `a7238c7` |
| `dba8cc3` | drop | Executable-name-based elevation grants every msiexec client and immediate action an elevated token. Windows elevates selected service actions, not arbitrary clients. Session-ID addition is independent and not necessary for retained fixes. | — |
| `edc8d41` | drop | Chrome 156 system-install/startup feature is explicitly dropped, including automatic Active Setup. Trace fix already in `16d2912`; arbitrary nested compiler scanning violates loader search order; starting Active Setup on every root program launch is not a Windows logon boundary, can block launches for ten minutes per stub and marks unsuccessful stubs current. Policy calls return fresh unowned mutexes rather than protecting shared policy state. | — |
| `52fdd90` | keep counts; drop SIDs | Reapply processor counts independently of dropped policy calls. Review correction: all sources now share host counts with a 32/64-bit affinity-mask cap; the inconsistent fixed four-CPU x86 model is removed. SID code truncates at 256 characters and folds only ASCII, with no Windows golden SID vectors. Shape/equality tests cannot establish derivation correctness. | `3145edb`, corrected by `c6b48b5` |
| `08c7a95` | drop | WIP cannot suspend self, increments the current thread count outside normal suspension, has no limit/error handling, and only tests one child thread. Cross-process suspension needs a scheduler/handle-channel design with nested-count, multi-thread, access, exit-race and memory-read tests. | — |

## Overlaps checked

Read `git log --oneline -80 macos`, `git log origin/main`, the historical
`63576dd` report, and searched current source with `git grep`. Queried
`gh pr list --repo mirkobrombin/raven --state all --search <topic>` for
SSPI, suspend, installer, and the combined compatibility topics.
Upstream PR #64 concerns Schannel protocol restrictions, not ANSI dispatch;
#47 concerns SetThreadContext, not whole-process suspension. PR #27 is integrated
as `6d72ff7` and #59 as `9744ff7` despite upstream PRs still showing open.

Keep `9329e5a`/`4b03841` compiler provisioning and `503b101` admission unchanged.
The new app-local compiler rule only admits same-width application DLLs using
`ProgramLibraries`; it does not scan unrelated version subdirectories or replace
System32 provisioning. Existing anonymous-token (`a9d4ec8`), NT-file (`2b4d593`)
and semaphore-transfer (`f4f3fd6`, `4d4f0b3`) work is preserved.

## Windows contracts used

- [MSI custom-action type 2](https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-type-2) documents CreateProcessAsUser/CreateProcess for deferred EXE actions; [custom-action security](https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-security) describes impersonation and elevation. Neither documents a NULL token. The NULL-token call motivating this fix was observed in Raven's Wine-derived MSI implementation (historical report), not established as a Windows MSI contract. Raven chooses its process primary token for that compatibility path. The [CreateProcessAsUserW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasuserw) page documents primary-token creation and GetLastError on failure, but does not establish NULL-token-to-caller behavior or ERROR_INVALID_HANDLE for our synthetic invalid value. The fixture asserts Raven's compatibility and validation choices; these are not claimed as Windows conformance evidence.
- [SO_EXCLUSIVEADDRUSE](https://learn.microsoft.com/en-us/windows/win32/winsock/so-exclusiveaddruse) and [reuse/exclusive rules](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse): configure before bind and prevent competing binds. This adapter retains BSD non-reuse enforcement; it does not implement all Windows wildcard/interface, cross-user or reuse-contender error distinctions.
- [InitSecurityInterfaceA](https://learn.microsoft.com/en-us/windows/win32/api/sspi/nf-sspi-initsecurityinterfacea): returns the ANSI SSPI dispatch table. Credential/context handles and binary attributes share the W engine; strings use Raven's existing CP1252 convention. Unsupported provider functions remain unsupported.
- [GetActiveProcessorCount](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getactiveprocessorcount): group number or ALL_PROCESSOR_GROUPS; failure returns zero with last error. Raven models one group; maximum equals active because it models no offline processors.
- [DeriveAppContainerSidFromAppContainerName](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-deriveappcontainersidfromappcontainername): valid profile SID returned through an allocated buffer released by FreeSid. The documentation does not validate the source's truncated ASCII-only hash algorithm; no approximate implementation is retained.
- [SuspendThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread): counted suspension, maximum count, rights checks, and no user-mode execution while suspended. The WIP's coverage does not establish these properties for a process.
- [MSI in-script options](https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-in-script-execution-options): deferred NoImpersonate actions run in system context. This does not justify elevating the entire client by filename.
- [DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order): application folder before System32 after KnownDLL and other special resolution; altered search uses the loaded module's directory, not arbitrary child directories.
- [EnterCriticalPolicySection](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-entercriticalpolicysection): protects policy processing; a fresh unowned mutex does not provide that contract.

## Validation

All builds use `-j8`; only this area's CTest names run, serialized through
`/tmp/raven-ctest-lock.sh`. No lldb, full-suite run or installer end-to-end run.
The WIDL regression now invokes the provisioner main entry point in its RPC ABI
probe mode, using the same WIDL command as component generation. It verifies both
generated client calling conventions and is registered in CTest.
A complete private installer rebuild is omitted: this targeted generator probe
checks the specific regression without rebuilding unrelated components.

Commands and results:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8 --target raven darwin_capture_permission_test \
  teardown_wait_fixture64 teardown_wait_fixture32 \
  process_creation_fixture64 process_creation_fixture32 \
  darwin_sspi_ansi64_fixture darwin_sspi_ansi32_fixture \
  darwin_socket_exclusive64_fixture darwin_socket_exclusive32_fixture \
  darwin_processor_count64_fixture darwin_processor_count32_fixture
cmake --build build -j8 --target raven darwin_app_compiler64_fixture darwin_app_compiler32_fixture
PATH=/Users/yount/Documents/Projects/raven-components/tools/bin:$PATH python3 scripts/test-installer-widl.py
/tmp/raven-ctest-lock.sh salvage-55e3-verified ctest --test-dir build -j1 --output-on-failure \
  -R 'native_darwin_(capture_permission|app_compiler_x(64|32)|process_creation_x(64|32)|teardown_wait_x(64|32)|sspi_ansi_x(64|32)|socket_exclusive_x(64|32)|processor_count_x(64|32))$'
RAVEN_DXMT_ROOT=/Users/yount/Documents/Projects/raven-components/dxmt \
  /tmp/raven-ctest-lock.sh salvage-55e3-provisioned-compiler ctest --test-dir build -j1 \
  --output-on-failure -R '^native_darwin_app_compiler_x64$'
git diff --check macos
```

Configuration and target builds passed. WIDL: both ABIs passed. Final area
CTest run: **13/13 passed**. Provisioned compiler comparison: **1/1 passed**;
the app-local sentinel also wins with the current real System32 compiler configured.
`git diff --check macos` passed. No x86 execution hang occurred.

An initial expanded socket test failed because the test forgot to clear its
read-back value before disabling exclusivity; corrected in `f266a57`. The first
processor-count test exposed the x86 dispatcher model; corrected in `c6b48b5`.
The final run above includes both corrections. An initial compiler fixture target
build required explicit CMake reconfiguration after adding new targets; the
reconfigured target build passed.

Evidence: `/tmp/raven-55e3-build.log`, `/tmp/raven-55e3-compiler-build-final.log`,
`/tmp/raven-55e3-verified-tests.log`, and
`/tmp/raven-55e3-provisioned-compiler-tests.log`. Existing compiler warnings remain;
no new runtime failure is hidden by continuation flags. Hardware/TCC integration
is deliberately mocked for the permission regression, so the test changes no
user permission state and opens no capture device.

## Review corrections

The fixed four-processor x86 model was a Raven deviation, never a Windows
contract. It is now removed. A single Darwin helper supplies the host count,
clamped to 32 logical processors for the x86 model's pointer-sized masks or 64
for x64. This one-group cap remains a runtime limitation on larger machines;
real WOW64 exposes host topology and supports additional Windows group semantics.
KUSER_SHARED_DATA, NUMBER_OF_PROCESSORS, GetSystemInfo/GetNativeSystemInfo,
active/maximum counts, topology, CPU-set information, NT process affinity and
processor power records use that model.

The [Winsock Boolean option contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-setsockopt)
permits zero to disable reuse; rejecting it on an exclusive socket was incorrect.
The adapter now accepts it. The [Microsoft bind table](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)
specifies WSAEACCES for a reuse-enabled contender on the same specific address
as an exclusive socket. Darwin still returns WSAEADDRINUSE in this case: this is
an explicit error-code deviation, tested as such. It still rejects the competing
bind. Reproducing every Windows error distinction across independent Raven
processes needs shared endpoint ownership, not a per-socket flag. The fixture
also binds another adapter's specific IPv4 address on the same port; a host
without a second IPv4 interface reports the socket test as skipped after checking
the other assertions. Both widths exercised a second interface on this machine.

AppContainer derivation is explicitly unsupported: it clears the output SID and
returns HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED), with last error 120.
No shape-only SID approximation is installed. ProgramLibraries checks bitness
(PE32/PE32+), not the PE Machine value; the compiler admission comment now says so.

The six W SSPI names identified in review appear in the dispatch table but have
no registered handlers in this checkout. The ANSI adapter now forwards to a W
handler if present and otherwise returns SEC_E_UNSUPPORTED_FUNCTION. The ANSI
fixture calls each of these slots and checks a CP1252 TLS target's UTF-8 SNI.

Namespace unification is deferred: raven_darwin spans compatibility headers,
audio and system services beyond this salvage. Renaming it would add unrelated
churn; new helpers retain the surrounding namespace conventions.

## Review-round results

Formatting commit `bffe8c2` remains an ancestor. No push or source-branch change.

| Finding | Disposition | Evidence / commits |
| --- | --- | --- |
| 1: ANSI SSPI | Fixed; rejected only the assertion that the six W handlers exist here. Register adapters with explicit unsupported fallbacks, guard the W table lookup, call all six slots, assert CP1252-to-UTF-8 SNI. | `fdc2bb0` |
| 2: processor counts | Fixed by removing the four-CPU deviation and using one helper. Assert shared page, environment, both system-info calls, active/maximum counts, affinity, both topology formats, CPU sets, NT affinity and power records. Preserve non-Darwin behavior. | `640be19`, `2e5b549`, `e12fa12`, `38514d5`; formatting `0d429de` |
| 3: sockets | Fixed zero-reuse rejection. Pin BSD INUSE versus Windows ACCESS as a deviation and test the reuse-enabled contender plus a second interface. Explicit skip only when no second interface exists. | `563c66a`, `1089289`, `a941f34`, `f4b348e` |
| 4: WIDL | Fixed CTest registration and main call-site coverage. A temporary copy reverting main to -m32/-m64 is rejected by the test. | `c3be647` |
| 5: SearchPath | Fixed extension detection to ignore dots in parent directories; qualified extensionless test passes at both widths. | `600bb5b` |
| 6: microphone | Added grant-then-open with mocked AudioQueue boundary, generous five-second bound, actual runtime Mach-O plist inspection; removed dead plist. | `703218f`, `63a217b` |
| 7: teardown | Added four-worker deadline case at both widths and malformed/short ps-row regression. | `8310184` |
| 8: docs / SID / compiler comment | Qualified undocumented NULL-token/invalid-handle behavior, cited MSI docs, explicitly dropped Chrome 156 system install/startup, added tested unsupported SID handler. PE bitness is checked by ProgramLibraries; Machine is not. | `5e7b652` |
| 9: namespaces | Deferred: broader than this salvage and unnecessary for correctness. | — |

Validation used `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` with the
provisioned WIDL directory on PATH, followed by `cmake --build build -j8 --target`
for raven and only the affected fixtures (capture permission, SSPI, processor
counts, socket exclusivity, AppContainer stub, process creation and teardown,
at both guest widths where applicable).

Final combined command:

```sh
/tmp/raven-ctest-lock.sh review-55e3-complete ctest --test-dir build -j1 --output-on-failure \
  -R '^native_darwin_(capture_permission|runtime_microphone_plist|installer_widl|socket_exclusive_x(64|32)|sspi_ansi_x(64|32)|processor_count_x(64|32)|appcontainer_stub_x(64|32)|process_creation_x(64|32)|teardown_(wait_x(64|32)|multi_x(64|32)|ps_rows))$'
```

Result: **18/18 passed**, no skips, in 16.09 seconds. After making unavailable
second-interface coverage explicit, rebuilt the changed fixtures with `-j8` and
ran:

```sh
/tmp/raven-ctest-lock.sh review-55e3-interface ctest --test-dir build -j1 --output-on-failure \
  -R '^native_darwin_socket_exclusive_x(64|32)$'
```

Result: **2/2 passed**, no skips. Both widths therefore bound a distinct adapter
address on the same port. `git diff --check bffe8c2` passed. No x86 hang occurred.
The WIDL call-site mutation probe ran only against temporary copied scripts,
changed main's WIDL command to -m32/-m64, and confirmed the test failed with an
AssertionError. It did not modify the tracked script or build any installer.

The initial runtime-plist test misread otool's word ordering; `63a217b` replaces
that decoding with a direct Mach-O section parser. All final checks above include
the corrected parser. Microphone tests mock authorization and AudioQueue; no
capture device or TCC state was touched. No full suite, installer, lldb, or
unowned-process cleanup was used.

Logs: `/tmp/raven-55e3-review-build-final.log`,
`/tmp/raven-55e3-review-build-nt.log`, `/tmp/raven-55e3-review-build-end.log`,
`/tmp/raven-55e3-review-complete-tests.log`, and
`/tmp/raven-55e3-review-interface-tests.log`.
