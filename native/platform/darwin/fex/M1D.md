# M1d implementation progress — milestone incomplete

Branch `t3/fex-modhost-execution` fast-forwarded from `macos` through `a22048c`
and `c46c333`. No push. This follow-up **does not deliver the execution milestone**.
There is no demonstrated OS limit, FEX design limit, or entitlement requirement
blocking the requested Tier 0 snippet. The remaining stop is unfinished Raven
implementation, not a qualified feasibility blocker. **NO-GO for M2**.

## Implemented and exercised

- An unentitled native arm64 `MAP_JIT` owner, cache invalidation, and a per-thread
  write-mode hook that preserves nested write scopes. Multiple mappings execute
  successfully. No entitlements, signing changes, or system settings were used.
- A native arm64-to-EC entry adapter for eight integer register arguments. It
  installs/restores x18 and preserves Darwin registers that EC may destroy.
- EC-to-native import stubs and a bridge preserving full q6–q15 and x18. Uniform
  64-bit stack parameters support up to sixteen integer arguments. The bridge
  copies only the declared number of stack arguments. Typed native signatures
  with narrow stack arguments and Windows varargs are **not** supported by it.
- Twenty-eight additional CRT imports: memory, ANSI string/ctype, math, errno,
  UTF-16 string and Windows 32-bit long conversions. No Darwin `wchar_t`/`long`
  substitution is used. Sixteen synchronization imports: SRW shared/exclusive
  locks, condition variables, PEB lock and address waits, including timeouts.
- The original eleven substrate import slots now receive callable adapter
  addresses rather than bare Darwin function pointers. Preflight resolves **55
  of 127 slots**, leaving **72 unresolved**. Missing functions remain missing;
  none is replaced with a success-returning fake.

Evidence is `modhost/m1d-evidence.json`. The host self-test exercises VM, heap,
recursive locks, UTF-16/long conversions, a call through a generated import
adapter, a two-thread SRW/condition-variable handoff, immediate address timeout
and an unequal-address comparison. The assembly fixture exercises sixteen
integer arguments and deliberately clobbers EC-volatile/Darwin-saved registers.
It is a synthetic ABI fixture, **not compiler-generated PE code or an x86 guest**.
FP/vector preservation is implemented in assembly but still needs explicit
adversarial tests. Math imports were resolved, not executed by the tests.

## Reproduction

```sh
python3 scripts/test-fex-modhost.py --substrate-only
python3 scripts/test-fex-modhost.py --evidence /tmp/fex-execution.json
```

The first command passes only the native substrate tests. The second deliberately
returns **exit 1** after printing the unresolved imports; the mapper itself
returns 3. It must not be cited as an execution pass. It neither makes PE pages
executable nor enters the module. The pinned provisioned M1c DLLs were reused;
FEX modules were not rebuilt or patched further in this follow-up.

The test is also registered from Darwin CMake only when `xtajit64.dll` is
provisioned under `RAVEN_FEX_MODULE_ROOT`. A standalone configuration avoids
building Raven or provisioning unrelated fixtures:

```sh
cmake -S native/platform/darwin/fex/modhost -B /tmp/raven-m1d-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/raven-m1d-cmake -j8
/tmp/raven-ctest-lock.sh fex-m1d-substrate ctest --test-dir /tmp/raven-m1d-cmake --output-on-failure
```

Recorded result: `fex_modhost_substrate` **1/1 passed** in a Release build;
assertions are explicitly enabled for the substrate tests. No execution test is
registered as passing. The Python driver's default mode continues to require
execution and fails until implemented. No Raven/bootstrap or x86 process ran.
The standalone arm64 ABI fixture was debugged with lldb after a test-fixture stack
layout error, corrected before the recorded passing run. FEXCore and its JIT
remain unchanged, as do production launch, Rosetta and graphics behavior.

## Still required

1. Resolve the 72 missing import slots listed in the evidence: loader lookup and
   image metadata, native executable VM allocation (`NtAllocateVirtualMemoryEx`),
   file/section/registry operations, thread/system information and lifecycle,
   context capture/restore/unwind, string conversion and varargs formatting,
   run-once and cross-process emulator work. Optional unsupported requests need
   defined failures; unwind/context restore must be real before exception tests.
2. Implement mapped PE execution, CHPE-native export redirection, auxiliary IAT
   handling and self-module dispatch slots. Current adapters have not been called
   from the actual xtajit64 PE image.
3. Construct TEB, PEB, TLS, CPU area, AMD64 context and module/emulator stacks;
   pass the versioned host table through actual process/thread init. Standalone
   `MAP_JIT` allocation is not yet connected to the module allocator's EC-code path.
4. Implement spill/dispatch/context-apply/reentry and the returning x64 snippet.
   The existing frontend still raises unsupported on guest syscalls. Then prove
   precise AV/repair/resume and nested callbacks in the requested order.

These are also the remaining NT/hosting obligations for real processes; broader
handle, I/O, VM splitting/decommit, cancellation, remote thread contexts, unwind,
SMC/invalidation and FP/vector semantics need qualification beyond a first snippet.
Synchronization state currently lives in process-owned side tables with process
lifetime; this is a standalone harness, not production object-lifetime management.
Tier 0 retains 16 KiB host pages and coarse 4 KiB protections as an explicit gap.

## Measurements and decision

No guest instruction, module init, dispatch, precise guest fault or callback has
executed. Therefore there are **no FEX compute-throughput or guest/host round-trip
measurements**. The prior SPIKE baseline remains integer 9.293 ms and float
12.525 ms per 10M iterations; it was not rerun. Test duration is not a translator
benchmark.

M2 (`FexProcess` behind `--execution=fex`) remains **NO-GO** until at least actual
module init and the verified returning guest dispatch execute, followed by fault
and nested-callback qualification. This progress does not meet the user's
single milestone or establish an allowed hard blocker.

## Continuation: strings and thread environment

Added sixteen string/bitmap imports (UTF-8 host code page, UTF-16 conversion,
counted strings, TLS bitmap operations and harness path conversion). Constructed
owned PEB/process parameters, TEB, CPU area, AMD64-context storage, static TLS
vector and demand-zero EC-code bitmap. Self-tests check Unicode surrogate pairs,
truncation boundaries, bitmap allocation and TEB/TLS/CPU-area wiring. Native
substrate tests pass. These objects are not yet passed to module init; the
remaining PE loader/import work continues. No new hard blocker is established.
