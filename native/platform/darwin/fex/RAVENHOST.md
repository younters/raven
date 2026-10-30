# M1c: Raven host frontend feasibility spike

Follow-up: [M1D.md](M1D.md) records the completed standalone execution milestone:
127 bound imports, module init, returning guest dispatch, precise AV recovery,
nested callback and verified compute/round-trip measurements. The M1c report below
is the historical build-only checkpoint.

**Partial implementation; NO-GO for M2.** The patched Windows modules compile and
pass their PE audits. No module initialization or guest execution was attempted:
the standalone loader stops with **116 unresolved import slots**. This is a
bounded engineering stop, not evidence that FEX cannot execute on Darwin.

Branch: `t3/fex-raven-host-frontend`. M1b commit `4c817b5` was cherry-picked as
`a22048c`. The M1c implementation commit is the commit introducing this report
(`git log -1 --format=%H -- native/platform/darwin/fex/RAVENHOST.md`). No push.
Production Raven, Rosetta, graphics, boot arguments and system settings were not
changed. No Raven/bootstrap or other x86 executable was launched. No entitlement
was claimed, debugger used, or process killed.

## Reproduce and evidence

From the Raven worktree:

```sh
python3 scripts/build-fex-modules.py --raven-host --build-only
```

All module builds use **-j8**. The external output root defaults to
`/Users/yount/Documents/Projects/raven-components/fex-modules/`:

- `FEX-darwin/build/local/m1c/fex-result.json` and `logs/`: final build and PE audits;
- `m1c-manifest.json`: pins, dependency archives, patch hashes and artifact hashes;
- `m1c-reproduce.log`: complete build-driver invocation and native substrate checks;
- `m1c-host-preflight.log`: every unresolved import, not just a count;
- `m1c-x18.log`: repeated native x18 probe;
- `m1c-native-host`, `xtajit64.dll`, `xtajit.dll`: native harness and modules.

A compact copy is committed in `modhost/m1c-evidence.json`. Binary files and full
build trees stay external. The initial M1c build is retained in
`FEX-darwin/build/local/m1c-initial`; it is not the final reproduction evidence.
The driver verifies the entire patch series with temporary git indexes, accepts
only pristine/exact-prefix/exact-final source trees, and rejects unrelated edits.
It checks the pinned source/toolchain archives and caches by source status.
Do not run the M1b build against the M1c-patched source tree: select Raven-host
mode or use a separate output root with its own pinned checkout.

Pins inherited from M1b:

- FEX-darwin: `370c63057fc9fc826b58fe15d59930876df55b5f` (FEX-2609.1-derived).
- llvm-mingw: `20260616-ucrt-macos-universal`, archive SHA256
  `2cab02a2e964bd4aae981150a45985d07c657cfa8d244959eb9e2dcc5eedd7b1`.
- Exact six dependency gitlinks and checksums remain in `MODHOST.md` and manifest.
- Final x64 DLL: 5,508,608 bytes, SHA256
  `35c666616cb817d41bb7b04c4f5b67a5b8502f6a4c1c59df830801846b60f6c8`.
- Final WoW64 DLL: 4,959,232 bytes, SHA256
  `da77711ed23034128bb53ef92eaf8f9bcf891692fbf05574b2980176b858aad9`.

The x64 image remains AMD64-machine with ARM64EC CHPE ranges and x64 export
thunks; it is not an ordinary ARM64 DLL. The audit confirms `RavenProcessInit`
is exported. WoW64's bytes are identical to M1b; it was built, not executed.

## Patch inventory and deliberately incomplete boundaries

Patches live in `patches/`, in application order. Only MIT FEX frontend/build
glue is changed; FEXCore, translator and JIT sources are untouched. Raven's new
code is original MIT code; no Wine implementation was copied.

1. `0001-deterministic-pe-timestamps.patch`: M1b's recipe change, relocated here
   for the M1c series. Removes wall-clock PE timestamps.
2. `0002-raven-host-frontend.patch`:
   - Windows CMake adds `RAVEN_HOST`, scoped to the ARM64EC frontend subtree.
     It does not define the switch on FEXCore or enable a WoW64 host mode.
   - `Common/RavenHost.h` defines the experimental versioned table and a native
     register-branch callback bridge, following the existing FEX UnixLib pattern.
     This avoids passing native callbacks through EC code classification. Its
     preservation of every required EC nonvolatile register is **not qualified**.
   - `Module.cpp` adds `RavenProcessInit`, validates required table fields, version,
     size, zero flags and guest-window alignment, and retains the host-owned table.
     Non-Raven builds export an explicit unsupported entry. The table is process
     lifetime; initialization is single-attempt and not retryable after failure.
   - Raven mode skips ntdll export/RVA syscall discovery, image-list tracking at
     startup, Wine environment synchronization, and KiUserExceptionDispatcher LUT
     lookup. CPU-area lookup and shared-data backing come from the table.
     Existing tracker objects and normal configuration/CRT initialization remain.
   - `Allocator.cpp` supplies write-protect/native-JIT hooks from the table.
     **ARM64EC's existing allocator uses NtAllocateVirtualMemoryEx for executable
     allocations; that import still needs a real MAP_JIT adapter.** Merely
     supplying allocate_native_jit does not qualify x64 JIT memory.
   - `FEXUnixLib.cpp` uses a direct native unix callback and a host handle token,
     instead of pointer exports and Wine's private UnixLib lookup queries.
     Optional file-to-fd mapping remains unavailable. Tier 0 must reject hardware
     TSO and preserve software barriers.
   - `Logging.cpp` routes messages through the native callback.
   - The Raven syscall branch explicitly raises STATUS_NOT_SUPPORTED. The
     dispatch and exception callbacks are **reserved contracts, not implemented
     hand-offs**. The current code must not be used as an execution backend.
3. `0003-raven-host-build-option.patch`: adds `--raven-host` to the downstream
   recipe and passes CMake's switch. Export and CHPE audits still run.

The init branch is build-qualified only. In particular, skipping `InitSyscalls`
also skips its self-module call-checker setup. Before invoking CRT constructors
or imports, the host must install the module's dispatch slots (or add a native
self-module setup path) and resolve the export's native entry through its **own**
CHPE metadata. Neither operation requires synthesizing an ntdll image, syscall
IDs, or Wine redirection tables. Neither operation is implemented in this loader.
The small native callback bridge does not solve the module's ordinary indirect
calls or Darwin/EC differences for stack arguments, varargs and nonvolatiles.

## Experimental interface

The exact shared definition is `modhost/raven_host.h` (also in the patch):

```cpp
struct RavenHostV1 {
  uint32_t size, version;
  uint64_t flags;
  uint64_t guest32_base;
  const void *shared_data;
  uint64_t (*dispatch)(uint64_t syscall, void *amd64_context);
  int32_t (*unix_call)(uint64_t handle, uint32_t ordinal, void *args);
  int32_t (*exception)(void *record, void *amd64_context, void *arm64_context);
  void *(*cpu_area)(void *teb);
  void (*log)(const char *message);
  uint32_t (*jit_write_protect)(uint32_t mode);
  void *(*allocate_native_jit)(uint64_t size);
  void (*memory_event)(uint64_t address, uint64_t size, uint64_t kind);
};
```

Version is 1, size must match exactly, flags must be zero. Callbacks are native
arm64, nonvariadic, at most three arguments, and may not throw across the boundary.
Context pointers designate the pinned Windows context structures, never Darwin
`ucontext_t`. The table remains host-owned for the lifetime of the module.
The exception/dispatch resume semantics and memory-event kinds are deliberately
not frozen; those fields are required placeholders and currently not invoked.
`guest32_base` is validated but not consumed by x64 initialization. This is not a
stable production ABI or an implemented 32-bit guest-window handshake.

A later dispatch bridge should reconstruct an AMD64 context after spilling JIT
state, advance RIP past SYSCALL/INT 2E, deliver the guest's ABI arguments to Raven,
apply the returned context, then reenter the dispatcher on its emulator stack.
It must test that a changed RAX survives resumed guest execution. Returning from
the current nonreturning EC hand-off as if it were an ordinary C function is not
an adequate substitute. A future callback requires a stack of guest entry frames.

ARM64EC fast calls remain possible: the existing DispatchJump/ExitFunctionEC and
related assembly are unchanged. Raven could generate EC entry/exit thunks for
selected native API implementations and classify them as native with a host-owned
code map, translating Windows arguments and preserving EC state. The call path
must distinguish these entries from generic Darwin function pointers. No Wine
ntdll image/LUT needs to be synthesized. Fast calls were not exercised here.

## Harness and goals

`native_host.cpp` extends the M1b mapper through an optional resolver. The mapper
bounds-checks section copies and DIR64 relocations and writes resolved IAT slots.
Mappings remain nonexecutable. It never attempts DllMain/RavenProcessInit,
constructs no TEB/PEB/CPU area, does not install x18 as a TEB pointer, and does not
pass the interface yet. Even zero missing imports would not authorize execution
in this implementation. The import resolver supplies **11 slots**, including
VM allocate/free/protect/query, heap allocate/free and recursive critical sections;
it also implements a QPC adapter (not imported by this final binary).

These are real but intentionally narrow substrate functions, not a complete NT
provider: anonymous high-address whole-region allocations, explicit failure for
unsupported requests, tracked memory queries, heap zeroing, actual protection
changes and recursive mutual exclusion. Commit into an existing reservation,
partial region splitting/decommit, guard pages and native executable allocation
are not implemented. VM semantics are 16 KiB and cannot claim Windows 4 KiB
accuracy. SRW, TLS and thread-information adapters remain absent. Native adapter
self-tests do not establish that the same functions are ARM64EC-callable.

| Goal / gate | Result | Evidence |
| --- | --- | --- |
| Patched DLL build and PE audit | PASS | final recipe status `pass`, both targets, -j8 |
| Repeated legacy-SDK x18 probe | PASS | 16 KiB pages; 100 sleeps, 26 signals; all mismatch counters zero |
| Native substrate checks | PASS | VM write/protect/query/release, zero heap, recursive lock, monotonic QPC |
| Map/relocate and partial import resolution | PASS | 11 resolved, 116 unresolved slots; expected exit 3 |
| 1: x64 module initialization under Tier 0 | UNTESTED / BLOCKED | loader stops before executing PE code |
| 2: guest dispatch, returned value, resumed completion | UNTESTED / BLOCKED | no guest entered; Raven syscall branch explicitly unsupported |
| 3a: precise AV and context repair/resume | UNTESTED | no Darwin exception bridge |
| 3b: nested harness-to-guest callback | UNTESTED | no resumable entry frame stack |
| 3c: throughput and round-trip latency | UNTESTED | no guest timing possible |

No FEX throughput, round-trip, initialization-time or Rosetta/FEX ratio was
measured. The prior SPIKE.md Rosetta baseline is integer **9.293 ms** and float
**12.525 ms per 10M iterations**, not rerun here. Native substrate checks are not
substitutes for these measurements. Guest-window/native-JIT/16 KiB guard changes
remain in the pinned source, but their presence is not runtime validation.
WoW64, SMC, x87 and YMM remain deferred. The new native/EC boundary will need
explicit vector and FP preservation tests before any conclusions about them.

## Concrete remaining work and effort

The full unresolved import list is in committed `m1c-evidence.json`; MODHOST.md
also catalogs the original 127-slot surface. Resolving every slot is necessary
but insufficient; optional APIs should fail with defined NTSTATUS and initialized
outputs where required, rather than succeed with invented state.

1. Complete NT substrate and callable ABI adapters: NtAllocateVirtualMemoryEx
   with native MAP_JIT ownership, reservation/commit/decommit/protection splitting,
   section/file operations, instruction-cache flush, SRW/condition/address waits,
   PEB locks, thread identity/query/context, TLS and Windows UTF-16/string/varargs
   CRT routines. Common configuration also probes files and registry. The generic
   Darwin libc symbols cannot all be assigned to EC import slots unchanged.
2. Build real TEB/PEB process parameters, TLS vector, loader information consumed
   by configuration, CPU-area pointer at TEB+0x1788, AMD64 context, emulator stack
   and suspend doorbell. The retained assembly still reads the pinned CPU-area
   offsets regardless of the new C++ lookup callback. Install x18 on each thread.
3. Implement callable ARM64EC export lookup and self-module dispatch slots; then
   exercise registration and first CRT/allocator initialization under an arm64
   debugger if necessary. Do not call an x64 export thunk as native code.
4. Implement syscall context spill/dispatch/resume plus one deterministic guest
   snippet. Wire exception and memory callbacks, using existing invalidation
   exports in the opposite direction. The current fields alone implement none of
   this. Native context restore must not issue the existing direct Windows SVC
   wrappers on macOS; replace those frontend boundaries with Raven callbacks.
5. Qualify SIGSEGV/SIGBUS/SIGILL/SIGFPE reconstruction, async-signal-safe transfer,
   nested entries, memory notifications, vector state and guest-window behavior.

Planning estimates, not measurements: **4–8 engineer-days** for the import/ABI,
TEB/TLS and PE entry gates; **3–7 more days** for first init and a returning guest
syscall; **2–4 further weeks** for exception recovery, nested callbacks,
invalidation and FP/vector qualification. High uncertainty remains until the
first PE instruction and first JIT block execute. A production FexProcess plus
Raven integration/parity remains a separate multiweek project, not a small wrapper.

**NO-GO for M2 now.** Continue a focused M1c follow-up on the unresolved import
and callable-entry gates, then demonstrate init and one verifiable returning guest
call before adding a production backend. Main risks are ARM64EC/Darwin ABI and
stack transitions, native JIT memory lifecycle under Tier 0, the legacy-SDK x18
workaround, signal/context restoration, and 16 KiB protection granularity. This
spike removes a build uncertainty for an isolated frontend overlay; it does not
remove the runtime feasibility gate identified by M1b.
