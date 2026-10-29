# M1b: FEX Windows modules in a native Darwin host

2026-10-29. **No-go for M2 on this evidence.** Gate 1 passes using the legacy
SDK linker setting; gate 2 passes for both DLLs. Gate 3 reaches non-executing
section mapping and relocation, then stops at the unimplemented NT/CHPE host.
This is the time-box engineering stop, **not a runtime demonstration that this
route cannot work**. No guest instruction has executed. The full standalone
host requested by gate 3 is not delivered; the committed sources are prerequisite
probes and a non-executing PE preflight mapper. All M1 guest goals remain untested.

## Scope and reproduction

Branch: `t3/fex-module-host-spike`. Starting Raven commit:
`a401264e669e3af48b14b621976e3bbeb99c245c`. Implementation SHA: obtain with
`git log -1 --format=%H -- native/platform/darwin/fex/MODHOST.md` (also in handoff).

Read the supplied Fable design and the previous worktree's `SPIKE.md`, including
MAP_JIT, high guest-window, protection and Rosetta probes. The module route here
is the user's revised experiment despite the original design rejecting it.
No production launch, Rosetta, graphics or NT implementation was changed. No
Raven binary, ctest, debugger, entitlement signing, boot-argument change or system
setting change was used. Only our own standalone arm64 programs ran.

Host: Apple M4 Pro, macOS 27.2 build 26B5091g, 16 KiB pages, Apple clang 21.0.0,
CMake 4.4.4. llvm-mingw's Darwin universal compiler runs natively; build script
checks `sysctl.proc_translated == 0`. Both module builds use `--jobs 8`.

```sh
scripts/build-fex-modules.py                  # prerequisites + modules + preflight
scripts/build-fex-modules.py --probes-only    # no download/build
scripts/build-fex-modules.py --build-only     # only repeat gate 2 + preflight
```

Default external root is `/Users/yount/Documents/Projects/raven-components/fex-modules/`;
`--root` overrides it. The script fetches the pinned source and toolchain, verifies
SHA256s, initializes only six pinned gitlink dependencies, applies the in-repo
patch, builds the two modules, audits them through FEX-darwin's recipe, and copies
`xtajit64.dll` and `xtajit.dll` into that root. Existing unknown source edits or
wrong HEADs are rejected. A blocked/partial build is not silently accepted; retain
its logs and use a fresh external root to retry. Cached successful artifacts are
checked against their recorded hashes and source/submodule identities.

The script's success means **prerequisites/build pass**, not M1b or guest execution
pass. Preflight's exit 3 is an explicit unresolved-import stop, checked separately
from its error exit 2. It never makes PE pages executable or calls their entrypoints.
All build products and raw logs remain outside Raven.

## Gate 1: x18 / TEB — PASS with a legacy SDK setting

The original design's statement that Raven does not need x18 is inapplicable to
this route: native Windows module code uses x18 for `NtCurrentTeb()`.

Independent MIT probe `modhost/x18_probe.c` writes marker `0x123456789abcdef0`,
spins (10 million reads), sleeps 100 times for 1 ms, then spins under periodic
1 ms SIGALRM delivery. The handler checks both live x18 and saved
`ucontext_t.uc_mcontext->__ss.__x[18]`. The probe restores the original register
before diagnostics. Compiler uses `-ffixed-x18`. Each variant runs as its own process.

Recorded reproduction (signal counts vary with scheduling):

| Link mode | Spin bad | Sleep bad | Signals | Saved x18 bad | Live x18 bad | Exit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current SDK/default target | 3322544 | 100/100 | 34 | 34 | 34 | 1 |
| `-mmacosx-version-min=11.0` | 0 | 100/100 | 25 | 24 | 25 | 1 |
| `-Wl,-platform_version,macos,11.0,11.0` | 0 | 0/100 | 26 | 0 | 0 | 0 |

The last signal count should be checked against `gate1/results.json` for the actual
run; the pass criteria require at least one signal, no saved-context corruption,
and the marker still present after return. Two separate early legacy-SDK runs
also passed. Default mode sometimes survives the short spin; sleeps and signals
expose the loss. These are scheduling/signal tests, not proof over all frameworks,
thread startup configurations, or future OS versions. Multithread contention was
not separately tested.

`otool -l` records `LC_BUILD_VERSION` SDK and minimum 11.0 only for the last variant;
lowering the deployment target alone still records the current SDK. The legacy
binary has default 4 GiB PAGEZERO, ad-hoc linker signature, and no entitlements.
It reports `os_custom_x18_abi_enabled() == true` already. No setter is needed.

Both current-SDK variants find the public API, report mode false, and terminate
with SIGTRAP (Python returncode -5) when `os_set_custom_x18_abi_enabled(true)` is
called without entitlement. The legacy variant's `--enable` control exits 2 because
it already reports enabled; it deliberately avoids toggling true twice.

Evidence and interpretation:

- athei Wine pin below, `dlls/ntdll/unix/signal_arm64.c:1768–1788`, dynamically
  looks up the two APIs; `server.c` calls its helper for initial/new threads.
  Its comment names `com.apple.security.custom-x18-abi-toggle`.
- Local Xcode SDK `usr/include/os/arch/arm64.h` declares availability 26.4. It
  requires toggling at native/custom ABI boundaries, prohibits ordinary macOS
  library/framework calls inside custom mode, and requires saving/restoring the
  custom register across toggles. Signal handlers must obey the same boundary.
  The custom-mode probe's handler only accesses memory, and its spin makes no
  macOS calls. The rejected API call never reached that spin in this experiment.
- Brendan Shanks's [CodeWeavers announcement](https://list.winehq.org/hyperkitty/list/wine-devel@list.winehq.org/thread/CKG5CEN2BE5VRXZ7O7NX4YUSBH3247WH/)
  describes the public mode in macOS 26.5 and gives the **exact legacy linker flag**
  used above. This is direct first-party evidence, not an inference from CrossOver
  packaging. No CrossOver binary was inspected.

Thus x18 is not an entitlement hard stop on this machine. Legacy SDK tagging is
a temporary compatibility mechanism, not a promise from Apple for macOS 28.
A real host must validate all its native callbacks, new threads and signal paths;
public-mode enable/disable boundaries should replace this workaround when usable.

## Gate 2: module builds — PASS

Used FEX-darwin, not the newer athei-FEX branch: it already contains guest-window,
shared-data, native-JIT, nested write-scope and 16 KiB guard changes for Tier 0.
Six dependencies were fetched at gitlink commits. Both complete builds and PE
audits passed with llvm-mingw 20260616, no Linux-core port patches.

Raven's sole patch `modhost/patches/0001-deterministic-pe-timestamps.patch` adds
`--no-insert-timestamp` to the recipe's PE linker flags. Both DLLs were rebuilt
with that recipe. This removes wall-clock COFF timestamps; it is not a claim that
all upstream build inputs are reproducible across machines. The initial unpatched
successful build was retained externally as `build/local/m1b-initial`.

The upstream recipe validates exports against each `.def`, DLL import sets,
Wine builtin marker, 16 KiB section alignment, and ARM64EC CHPE code maps. x64's
on-disk machine is **AMD64 0x8664**, with ARM64EC/CHPE metadata and both ARM64EC
and x64 code ranges. WoW64's machine is ARM64 0xaa64. `ld.lld` emits its generic
`/align specified without /driver; image may not run` warning; the audit confirms
the intended 16384-byte alignment. Successful linkage does not prove execution.

Artifacts, pins and dependency checksums are listed below. External build evidence:
`FEX-darwin/build/local/m1b/fex-result.json`, `logs/{arm64ec,wow64}-{configure,build,pe-audit}.log`,
`manifest.json`, `reproduce-build.log`, and `verify.log`.

## Gate 3: standalone host — TIME-BOX STOP / INCOMPLETE

`modhost/pe_preflight.cpp` is original C++, built native arm64 with
`-std=c++17 -O2 -Wall -Wextra -Werror`. It bounds-checks and copies PE sections into
non-executable memory, applies DIR64 relocations, enumerates import thunks, and
stops explicitly. It is not a general hardened PE parser or an executable loader.

| DLL | Sections | Image bytes | DIR64 fixups | Unresolved import slots |
| --- | ---: | ---: | ---: | ---: |
| xtajit64 | 9 | 4325376 | 19187 | 127 |
| xtajit | 7 | 4030464 | 18930 | 133 |

Both preflights return 3 as designed. None of the imports is resolved; no native
NT exports, TEB/PEB, DLL process attach, simulation or exception bridge is installed.
This establishes only the first half of the mapping work. A generic arm64 PE
loader plus fake success-returning NT stubs would not establish feasibility.

Concrete initialization blockers found in the pinned module source:

1. **x64 is ARM64EC, not the WoW64 BTCpu interface.** Its exports are `ProcessInit`,
   `ThreadInit`, data slots `BeginSimulation`, `DispatchJump`, `RetToEntryThunk`,
   `ExitToX64`, and `ResetToConsistentState`. Entering an export's x64 thunk as
   arm64 is invalid. CHPE code map/redirection and EC call-checker setup must be
   implemented, including callable pointer/data-export distinctions. The WoW64
   DLL instead exports `BTCpuProcessInit`, `BTCpuThreadInit`, `BTCpuSimulate`,
   `BTCpuResetToConsistentState` etc. The requested shared BTCpu sequence cannot
   simply be applied to xtajit64.
2. `ARM64EC/Module.cpp::ProcessInit` starts with `InitSyscalls`, walks ntdll's PE
   exports, builds redirection tables, patches the call checker and derives Wine
   syscall IDs by sorting export RVAs. A C++ name-to-function resolver alone
   cannot meet this requirement. Raven must synthesize a coherent ntdll PE image,
   CHPE metadata/LUTs and dispatch stubs, or deliberately patch the module's
   frontend interface. No such patch was attempted here.
3. `ThreadCPUArea` reads a `CHPE_V2_CPU_AREA_INFO*` at **TEB+0x1788**, including
   `ContextAmd64`, emulator stack bounds, `InSimulation`, `InSyscallCallback`,
   `SuspendDoorbell` and `EmulatorData[0..3]`. PEB image base, loader module lists,
   process environment and NT thread context queries must agree with that layout.
   WoW64 uses `RtlWow64GetCurrentCpuArea` and its own context/TLS state; its
   process init requires successful class **1010** guest-base query, even when
   supplying base zero. A high reservation alone is insufficient.
4. The x64 syscall path expects `__wine_syscall_dispatcher` as a **pointer-valued
   export**, not a direct ordinary C function. Unix dispatcher is likewise read
   through an exported pointer, with an EC trampoline bypassing normal call
   checking. Nested returns and EC register/stack conventions need assembly
   boundaries rather than Darwin's variadic/C ABI defaults. Windows WCHAR is
   16-bit, LONG is 32-bit, and ARM64EC has special register/context rules.
5. A Darwin signal context is not a Windows context. Wine's
   `signal_arm64ec.c::dispatch_emulation`/exception paths drive BeginSimulation
   and ResetToConsistentState using separate ARM64 and AMD64/EC contexts.
   Raven must translate SIGSEGV/**SIGBUS**/SIGILL/SIGFPE, classify JIT versus
   native faults, reconstruct state, honor the resulting PC/SP, and resume.
   Calling arbitrary allocating NT functions directly from a POSIX handler is
   not a valid substitute. Nested callbacks and vector state must be tested.

**Stop reason:** writing and qualifying this private NT/ARM64EC hosting protocol
is beyond a bounded build/prerequisite spike. This is an unimplemented engineering
gate, not a failed runtime initialization or an assertion of impossibility.
No later gate was attempted after this stop. Original source only is committed;
no LGPL Wine implementation was copied. Source references describe the protocol.

Planning estimates (engineering judgment for someone familiar with both runtimes):
PE/CHPE mapping + ntdll image/dispatch and initial CRT/VM/heap/thread surface,
**1–2 weeks**; first x64 init and controlled stop/resume, **another 1–2 weeks**;
exception recovery, nested callbacks, SMC and full FP state, **another 2–4 weeks**;
WoW64/high-window integration **1–2 additional weeks**. Overlap is possible and
failures behind init can change these estimates. A smaller separately authorized
frontend patch that bypasses Wine's ntdll/CHPE assumptions deserves comparison.

## Gate 4 / M1 goals and measurements

| Goal | Result |
| --- | --- |
| x64 guest execution, stop and resume | UNTESTED |
| precise AV, divide error, illegal instruction and resume | UNTESTED |
| nested host → guest callback | UNTESTED |
| 32-bit guest with FS/TEB | UNTESTED |
| guest and host self-modification with invalidation | UNTESTED |
| x87 and YMM preserved across fault | UNTESTED |
| compute throughput versus Rosetta | NOT MEASURED |
| guest → host round-trip cost | NOT MEASURED |

The previous SPIKE.md baseline remains integer **9.293 ms** and float **12.525 ms**
per 10 million iterations. It was not rerun; no x86 process was launched here.
There are no FEX throughput numbers or ratios. Native relocation/probe timing is
not a substitute for guest execution measurements.

## Gate 5: Tier 0 protection policy — documented gap

Exact 4 KiB subpage semantics are deferred to entitled Tier 1 as requested.
Previous native probes show that a 4 KiB protection at a 16 KiB-aligned address
rounds outward across the entire host page; unaligned slices fail EINVAL.
A future Tier 0 NtProtectVirtualMemory adapter must report this known limitation:
outward rounding can deny legal accesses or permit illegal ones depending on
chosen permissions. This spike does not implement subpage emulation, claim exact
GUARD semantics, or use this gap to reject the Tier 0 build experiment.
Previous MAP_JIT and high-window probes passed; they were read, not reclassified
as guest execution evidence. Tier 0 must retain software TSO and relocated shared
data; a future entitlement is not counted as a passing result here.

## Exact host surface

The following is the **observed static import surface of these built binaries**,
not all entries in the much larger `Defs/ntdll.def` import library. Runtime lookups
and image-layout protocols below are additional requirements. Some optional APIs
can return explicit unsupported status, but fake successes are unsafe. Darwin
libc aliases also need Windows/EC ABI adapters, especially varargs, context,
`__chkstk`, `_errno` and UTF-16 functions.

ntdll.dll imports common to both (123):

```text
LdrDisableThreadCalloutsForDll LdrGetDllFullName LdrGetDllHandle LdrGetProcedureAddress
NtAllocateVirtualMemory NtAllocateVirtualMemoryEx NtClose NtCreateFile NtCreateSection
NtCreateThreadEx NtDelayExecution NtDuplicateObject NtFlushInstructionCache NtFreeVirtualMemory
NtGetContextThread NtGetCurrentProcessorNumber NtLockFile NtMapViewOfSection NtOpenFile
NtOpenKeyEx NtProtectVirtualMemory NtQueryAttributesFile NtQueryDirectoryFile
NtQueryInformationFile NtQueryInformationThread NtQueryObject NtQuerySystemInformation
NtQuerySystemTime NtQueryValueKey NtQueryVirtualMemory NtReadFile NtSetInformationFile
NtSetInformationThread NtSuspendThread NtTerminateProcess NtTerminateThread NtUnlockFile
NtUnmapViewOfSection NtWaitForSingleObject NtWriteFile RtlAcquirePebLock
RtlAcquireSRWLockExclusive RtlAcquireSRWLockShared RtlAllocateHeap RtlCaptureContext
RtlClearBits RtlCreateUnicodeStringFromAsciiz RtlDeleteCriticalSection
RtlDosPathNameToNtPathName_U RtlEnterCriticalSection RtlFindClearBitsAndSet RtlFreeAnsiString
RtlFreeUnicodeString RtlGetCurrentDirectory_U RtlImageDirectoryEntryToData RtlImageNtHeader
RtlInitAnsiString RtlInitUnicodeString RtlInitializeConditionVariable
RtlInitializeCriticalSection RtlInitializeSRWLock RtlLeaveCriticalSection
RtlLookupFunctionEntry RtlMultiByteToUnicodeN RtlMultiByteToUnicodeSize RtlNtStatusToDosError
RtlQueryPerformanceCounter RtlQueryPerformanceFrequency RtlReleasePebLock
RtlReleaseSRWLockExclusive RtlReleaseSRWLockShared RtlRestoreContext RtlRunOnceExecuteOnce
RtlSleepConditionVariableSRW RtlTryAcquireSRWLockExclusive RtlTryAcquireSRWLockShared
RtlTryEnterCriticalSection RtlUTF8ToUnicodeN RtlUnicodeStringToAnsiString
RtlUnicodeToMultiByteN RtlUnicodeToMultiByteSize RtlUnicodeToUTF8N RtlUnwindEx RtlVirtualUnwind
RtlWaitOnAddress RtlWakeAddressAll RtlWakeAddressSingle RtlWakeAllConditionVariable
RtlWakeConditionVariable _errno _vsnprintf _vsnwprintf atan2 cos isalnum isspace isxdigit
memchr memcmp memcpy memmove memset sin sprintf sscanf strchr strcmp strlen strncmp strtol
strtoul swprintf tan tolower toupper vsprintf wcschr wcscmp wcscpy wcslen wcsncmp wcstol
wcstoul
```

Additional x64 ntdll imports:

```text
ProcessPendingCrossProcessEmulatorWork RtlIsEcCode RtlLocateExtendedFeature RtlRaiseStatus
```

Additional WoW64 ntdll imports:

```text
NtContinue NtQueryInformationProcess NtRaiseException RtlWow64GetCurrentCpuArea
RtlWow64GetThreadContext RtlWow64SetThreadContext RtlWow64SuspendThread __chkstk
```

WoW64 wow64.dll imports:

```text
Wow64ProcessPendingCrossProcessItems Wow64SystemServiceEx
```

Additional dynamic hooks and layout contracts, from grepping both the frontend
and Common sources (not limited to literal import thunks):

| Name / protocol | Required behavior or explicit optional fallback |
| --- | --- |
| `__wine_jit_write_protect` | uint32-style hook; query mode 2 must advertise capability (UINT32_MAX means unsupported); write/execute transitions and nesting must match `JitWriteScope` |
| `__wine_allocate_native_jit` | native executable cache allocation compatible with MAP_JIT and the module allocator's release/protection behavior; guest RX memory is distinct |
| `__wine_get_user_shared_data` | real readable backing for relocated KUSER_SHARED_DATA, used by x64 access emulation |
| `__wine_syscall_dispatcher` | exported pointer used by EC syscall dispatch and ntdll RVA-derived syscall tables |
| `__wine_unix_call_dispatcher` | exported pointer; `(uint64_t handle, unsigned ordinal, void *args) -> NTSTATUS`; EC direct trampoline bypasses its call checker |
| `__wine_dbg_output` | optional `void(const char *)` logging hook; no other `__wine_dbg_*` symbols occur in the built imports or this frontend logging lookup |
| `__wine_set_unix_env` | optional environment synchronization hook, used here for a driver configuration variable; Windows process environment must still exist |
| `wine_get_version` | Wine detection; affects overcommit, features and shared-data paths; exposing it changes behavior |
| `wine_server_handle_to_fd` | optional disk-cache mapping support; NT handle must become a valid Unix fd with proper lifetime |
| `LdrSystemDllInitBlock` | data-export lookup used for module addresses/WoW64; not a function |
| `KiUserExceptionDispatcher` | ntdll export and x64-to-EC redirection target; Windows exception frame/dispatch contract |
| `NtQueryInformationProcess(1010)` | guest-window base handshake; WoW64 init requires success and either zero or 4 GiB-aligned base |
| `NtQueryVirtualMemory` Wine classes | `MemoryWineLoadUnixLibByName` returns module/handle pair, fallback `MemoryWineUnixFuncs` returns handle; explicit failure means UnixLib unavailable |
| `RtlIsEcCode`, ntdll CHPE metadata | code classification, EC dispatch/call checking, x64 stub redirection |
| memory notification exports | allocation/protect/unmap/dirty/read-file and instruction-cache callbacks drive invalidation; host writes must notify too |

FEXUnixLib ordinals at this pin: **0** SetHardwareTSOControl (1-byte bool),
**1** SetKernelUnalignedAtomicControl (8-byte flags), **2** Madvise (24-byte
address/size/advice struct), **3** SetVMAName (24-byte address/size/name), **4**
GetSHMStatsVMA (16-byte base/map/max), **5** DeleteSHMStatsFile (no arguments),
**6** MapFile (24-byte fd/padding/size/result). Exact layouts and padding are in
`Source/Windows/UnixLib/FEXUnixLib.h`. Disable optional statistics/disk cache for
first bring-up; return failure for unavailable hardware TSO so software barriers
remain active. The shipped UnixLib implementation has Linux system dependencies;
it is not a ready-made Darwin dylib for Raven. Its small ordinal contract can be
implemented directly in Raven's own host adapter. These fallbacks still need
execution tests; they are not qualified by source inspection.

NT obligations include real virtual-memory allocation/protection/query and section
operations, correct heap allocation, UTF-16 and ANSI conversion, PE image queries,
locks/condition variables/address waits, thread/context lifetime and unwind/restore
semantics, file/registry queries used by config and CPU discovery, and coherent
performance/system time. Unsupported optional file/registry probes need meaningful
NTSTATUS failures, never success with uninitialized output buffers. The static
import set alone understates the work: the module parses ntdll and reads process
structures rather than going exclusively through imported calls.

## Pinned inputs and SHA256s

SHA256 source values below mean `git archive --format=tar <commit>` (tracked
source, gitlinks recorded; submodule contents hashed separately). Reference-only
Wine and athei-FEX clones were read, not linked or copied into Raven.

| Input | Commit / release | Archive SHA256 |
| --- | --- | --- |
| [FEX-darwin](https://github.com/dbc-hbin/FEX-darwin) | `370c63057fc9fc826b58fe15d59930876df55b5f` | `1e2dcfc3e7946f9a6db664e50fc2f9513060f0663c12ddbbeaeba336cabf9fcd` |
| [athei-FEX reference](https://github.com/athei/FEX) | `e5ac8008928062e3e6c2022847e3899e9bb3873c` | `0ee0b0a9f37f798844927419c7838efa7a74b00b9fc9c5468c7b4e8cabe6454c` |
| [athei-Wine reference](https://github.com/athei/wine) | `00039fa4d4f7b30c0d6613e5a44411d6750bb883` | `90007ac615f167976e09647a83f577aa75621ad7de4f55cb855a5399cc8f0415` |
| [llvm-mingw binary](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616) | `20260616-ucrt-macos-universal` tar.xz | `2cab02a2e964bd4aae981150a45985d07c657cfa8d244959eb9e2dcc5eedd7b1` |

FEX-darwin provenance names upstream FEX-2609.1
`9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf` as its baseline; the **actual built
snapshot** is the FEX-darwin pin above, not vanilla FEX-2609.1 or newer upstream
`d321f49`. Its MIT Darwin patches are already applied in that snapshot.

| Dependency | Gitlink commit | Archive SHA256 |
| --- | --- | --- |
| External/fmt | `c07e2aa4b130b4fd359a1c6455a5201b17af01fa` | `51603b49eedb909c1cc8b2e1bfddfdd8400d839d32df4c7120bcd671714f8179` |
| External/range-v3 | `ca1388fb9da8e69314dda222dc7b139ca84e092f` | `ac819bbc470077f1387e922f69edd1fd0016a940b7a145c5a030d01a90ae0982` |
| External/rpmalloc | `09142d726429416bfa7b459151515fe3ab7622dd` | `51d4c1f0e92091cea1087f36e36553270572e32acf70b51861ebc9c9e6923be8` |
| External/unordered_dense | `3234af2c03549bc85656bfd3a86993bf1cd8aef1` | `0eb7e4ed58a34e4deea34c121f5954f07d595a7de72d79d5f337e09a925e0ce5` |
| External/xxhash | `e626a72bc2321cd320e953a0ccf1584cad60f363` | `e776118585772cdec089191339575ac706e34bc795ce505f370be4e704c69435` |
| Source/Common/cpp-optparse | `9f94388a339fcbb0bc95c17768eb786c85988f6e` | `18a4f460fdc6a7b1c3aab0b4a9d325b593362039ff4941c0a356daff5dcef562` |

Final patched artifacts:

| File | Bytes | SHA256 |
| --- | ---: | --- |
| xtajit64.dll | 5518336 | `148d8980aa8e8c996ccc431e7c1595ae910c7c4051773c9c5e940dd69f418d41` |
| xtajit.dll | 4959232 | `da77711ed23034128bb53ef92eaf8f9bcf891692fbf05574b2980176b858aad9` |

Raven patch SHA256: `bd6e3165a3d1fbcb45155bee49a5b0242f38fdc6d11c1bed51ef7883ca657e46`.

`manifest.json` additionally records upstream patch checksums, source-file checksums,
licenses, compiler versions, command lines, PE exports/code maps and build logs.
Those hashes identify this build; the report does not promise identical DLL bytes
on another host/toolchain install. In-tree FEX MIT notices remain in external
sources and license bundles. New Raven probe/mapper code is MIT; the one build
recipe patch modifies MIT FEX-darwin code. No Wine LGPL source was incorporated.

## Comparison with direct FEXCore and M2 decision

The direct-core M1 attempt never compiled: 64 failed build steps shared Linux-only
headers and synchronization/VM/signal assumptions. This route **does compile** the
translator on its existing `_WIN32` paths, with downstream MAP_JIT hooks and guest
window machinery available. It removes that specific compilation blocker and is
a worthwhile alternative to keep investigating.

It replaces native-core porting with an NT/Wine frontend compatibility obligation.
Raven already owns NT semantics, but module imports must be native ARM/EC-callable,
and the private ntdll PE/CHPE layout, TEB CPU area, dispatch, context and unwind
protocols do not follow automatically from Raven's existing trapped provider DLLs.
Both routes still need qualified Darwin signals, nested guest entry, invalidation
and FP/vector recovery. The module route additionally couples Raven to downstream
frontend ABI versions. Tier 0's coarse protections remain an explicit shared gap.

**No-go for M2 (`FexProcess` behind `--execution=fex`) now.** Gates 1 and 2 remove
important uncertainties, but neither translator execution nor reliable return to
Raven has been established. Continue with a separately bounded M1c targeting only
x64 module initialization and one guest stop/resume, with an explicit choice
between synthesizing Wine-compatible CHPE/ntdll structures and an MIT frontend
patch that accepts Raven-owned dispatch/context callbacks. Reopen M2 after actual
execution, precise fault/resume, nested callback, SMC and x87/YMM evidence. Main
risks are private ARM64EC protocol complexity, legacy-SDK x18 longevity, Darwin
exception/ABI crossings and Tier 0 page granularity. Pending entitlement availability
may improve x18/VM policy but does not implement the missing hosting layer.
