# M1d: native FEX module execution

**The execution milestone and all four requested follow-ons pass in the standalone
arm64 harness.** All 127 xtajit64 import slots bind. Native module process/thread
init, a returning x86-64 dispatch, precise AV repair/resume, a nested callback,
compute verification and round-trip measurements execute under Tier 0.

**GO for M2 development:** build an opt-in `FexProcess` behind `--execution=fex`.
General Windows compatibility and a default switch remain unqualified. The NT,
unwind, signal, memory and vector-state work below is part of M2's qualification,
not something this small harness proves. No production launch, Rosetta or graphics
code was changed. Nothing was pushed.

Branch: `t3/fex-modhost-execution`, continuing `a22048c` and `c46c333`.
Implementation commits are recorded by:

```sh
git log --oneline c46c333..HEAD
```

## Evidence and reproduction

Committed evidence: [modhost/m1d-runtime-evidence.json](modhost/m1d-runtime-evidence.json).
It includes artifact/patch hashes, all five samples, exact runtime output, result
checks and the locked CTest outcome. Earlier `m1d-evidence.json` and
`m1d-continuation-evidence.json` preserve historical partial checkpoints.

```sh
python3 scripts/build-fex-modules.py --raven-host --build-only
python3 scripts/test-fex-modhost.py --evidence /tmp/fex-runtime.json

cmake -S native/platform/darwin/fex/modhost -B /tmp/raven-m1d-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/raven-m1d-cmake -j8
/tmp/raven-ctest-lock.sh fex-m1d-runtime ctest --test-dir /tmp/raven-m1d-cmake --output-on-failure
```

The build driver validates the exact pinned source/patch series, toolchain and
artifacts. It builds both modules with **-j8**, performs PE export/CHPE audits,
and copies them into the external root. Final fresh recipe output:

`/Users/yount/Documents/Projects/raven-components/fex-modules/FEX-darwin/build/local/m1d-final/`

The root also contains `m1d-manifest.json` and `m1d-native-host`. Native substrate
checks in the build driver are separate from execution; the second command above
requires actual init, resumed guest results, fault/callback markers and all timing
samples. A preflight stop or a zero exit without those checks cannot pass.

CMake registers `fex_modhost_substrate` and `fex_modhost_execution` only when
`xtajit64.dll` exists under `RAVEN_FEX_MODULE_ROOT` (the external root by default).
The same directory works standalone, without building Raven or provisioning its
unrelated fixtures. Release tests explicitly retain assertions. Final locked
CTest result: **2/2 passed**. `--substrate-only` selects only the native tests.

Pins remain FEX-darwin **370c63057fc9fc826b58fe15d59930876df55b5f** and llvm-mingw
**20260616-ucrt-macos-universal**. Final audited DLL SHA256s:

- `xtajit64.dll`: `94d3a55a034eaf36fffe56e866c97f5eca2e6744ee563ff33c7e8e9688fdeb1b`
- `xtajit.dll`: `010c342d2a10e73ac2a3029d3120d801eba8059341316728b836756977d09c6e`

WoW64 was built/audited, not executed. This is a native arm64 process on the same
M4 Pro/macOS 27.2 host, with 16 KiB pages and the legacy-SDK linker flag preserving
x18. No entitlement, signing-policy change, boot-argument change or system setting
was used. Debugging used only the standalone arm64 harness. The x86 benchmark
assembly is compiled into an object for byte verification; no x86 executable or
Raven/bootstrap process was launched.

## What runs

| Contract | Observed result |
| --- | --- |
| Imports | 127/127 bound; both ordinary and auxiliary IAT populated |
| Native entry | Native target selected from the DLL's own CHPE redirection table; final RavenProcessInit RVA `0x5bc4` |
| Process/thread initialization | DllMain attach, RavenProcessInit and ThreadInit all execute and pass |
| Returning dispatch | Guest arguments 7, 11, 13, 17; native return 224; resumed guest adds 0x1234 and verifies **4884** |
| Precise AV | Exact guest RIP and inaccessible allocation address checked; harness skips the three-byte load and sets RAX=0x1234; resumed guest verifies **4665** |
| Nested callback | During dispatch, native harness enters a guest callback with argument 41; callback returns 124; outer guest resumes and verifies **5008** |
| Compute | Both 10M-iteration kernels match the original SPIKE assembly bytes; integer result and float bits verified |
| Round trip | Each of five samples verifies 100,000 guest/native calls and the accumulated guest result |

The basic snippet performs one business dispatch plus a separate completion
syscall. Completion verifies the exact resumed end RIP and transfers to the saved
native test frame. It cannot pass merely by reaching the host once.

The host owns PEB/process parameters, TEB, runtime TLS slots/bitmap, a static TLS
vector, AMD64 context, CHPE CPU area and EC code bitmap. This DLL has no PE TLS
directory; the zero-size static template is explicitly observed. Its runtime TLS
allocation is used by the real CRT. x18 designates the TEB while executing EC code.
Both module and dynamically allocated native EC-code pages are classified in the
bitmap. Imports use adapters preserving EC vector nonvolatiles and x18; entry
adapters preserve Darwin registers that EC may clobber.

Native code uses MAP_JIT and nested write scopes. Immutable import veneers use
ordinary RX mappings so they remain executable while FEX enables JIT writes.
Guest executable pages contain x86 data and do not become native executable pages.
The host commits FEX's explicitly marked native overcommit reservations through
Darwin demand-zero mappings. Arbitrary reservations remain inaccessible.

The AV bridge converts Darwin signal state to the pinned ARM64 context. A small
frontend export asks FEX to reconstruct the precise guest state, delivers it to
the Raven exception callback, and returns a native context that reenters the
module's dispatcher with the repaired AMD64 packet. It does not issue a Windows
SVC or reinterpret a Darwin ucontext as a Windows context.

A nested entry saves the outer context and completion frame and uses independent
emulator/guest stacks. The same FEX thread state is then restored before the outer
dispatch returns. This demonstrates a synchronous single-level integer callback;
arbitrary-depth callbacks, callbacks inside waits and FP/vector nesting still
need qualification.

## Measurements

Five samples, 10,000,000 serial dependent iterations each. Function bodies are
byte-identical to the earlier `SPIKE.md` kernels: integer multiply/add recurrence,
SSE2 scalar multiply/add with no FMA. The driver independently assembles
`bench_kernels_x86_64.S` and compares the bytes in `bench.h` before execution.

| Median | Prior Rosetta baseline | FEX Tier 0 | FEX / Rosetta |
| --- | ---: | ---: | ---: |
| Integer, 10M iterations | 9.293 ms | **20.539 ms** | **2.21×** |
| Float, 10M iterations | 12.525 ms | **25.608 ms** | **2.04×** |

Verified integer result: `5292597211099207303`. Verified float result:
`4295705.5919631328`, bit-identical to the native C++ reference with
`-ffp-contract=off`.

Guest/native round-trip median: **118.199 ns/call**, five samples of 100,000 calls.
The range was 116.834–120.868 ns/call. This includes the guest loop, syscall state
spill, native dispatch, context application and dispatcher reentry. It is not an
isolated native function-call latency or an ARM64EC fast-call measurement.

Raw compute samples in ns:

| Sample | Integer | Float |
| --- | ---: | ---: |
| 0 | 20,417,042 | 25,607,666 |
| 1 | 20,685,000 | 25,667,291 |
| 2 | 19,947,375 | 25,675,500 |
| 3 | 20,539,250 | 25,607,000 |
| 4 | 20,627,000 | 25,199,459 |

Sample 0 includes first compilation of that benchmark's blocks; the other samples
reuse them. Timings cover guest entry through verified completion; the wrapper
adds a call/return and terminal syscall around the identical compute function.
The Rosetta baseline was not rerun. These are same-machine historical comparisons
under concurrent development load, not controlled whole-application benchmarks.
Both compute medians exceed twice the supplied baseline; M2 must measure real
workloads before a default decision. These register-only loops do not establish
that hardware TSO alone would close the compute gap.

## Patch and implementation boundaries

All Raven adapters are original code. No Wine implementation was copied or linked.
FEX notices remain intact; the build recipe bundles the upstream license inventory.
The six isolated patches apply in order to the pinned MIT FEX/FEX-darwin snapshot:

1. Deterministic PE timestamps (inherited).
2. Raven host frontend/table (inherited).
3. Raven build option (inherited).
4. Actual syscall spill/dispatch/reentry, nonnull allocator hooks and explicit
   native-overcommit notifications.
5. Native temporary JIT guard sizing. This is the necessary **Core allocation
   exception** to keeping Core untouched: the pinned temporary buffer uses a 4K
   guard, and rounding it to 16K protected the entire initial buffer. Buffer sizing,
   guard placement and fault bounds now agree on 16K when the Darwin write hook is
   installed. No guard check is removed and no guest instruction lowering changes.
6. Raven-owned precise fault entry and shared context-packet construction.

The first guard failure was observed under standalone lldb on the first emitted
instruction. Other corrected runtime failures were a null optional allocator hook,
executing a MAP_JIT import veneer during a write scope, privileged CTR_EL0 access
(replaced by public cache-line queries), and the inherited two-argument binding
for the one-argument RtlQueryPerformanceCounter API. None was hidden by a passing
stub or by ignoring a failure.

The host registry exposes a conservative CPU-feature model projected from Darwin
capability queries; unexposed identification values fail rather than inventing a
MIDR. String conversion uses a UTF-8 host code page and Windows UTF-16/32-bit-long
widths. File handles, reads/writes, basic queries, byte locks and section mappings
use actual native resources. Unsupported requests return explicit status failures.

The original unwind decoder follows the documented
[ARM64 PE unwind format](https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling).
It has context-capture and synthetic full-xdata tests, supports canonical packed
body frames, and rejects unsupported cases. It is not a complete native Windows
exception runtime.

## Remaining NT surface and M2 decision

- Native C++/SEH unwinding needs language-handler cleanup, complete packed partial
  prologs/epilogs and context flags/FP control semantics. RtlUnwindEx currently
  rejects cleanup handlers; unhandled RtlRaiseStatus reports the status and aborts.
  Binding those imports is not a claim that arbitrary exception paths pass.
- File sharing/access policy, general Windows drive/path rules, locale/code pages,
  broader file/object/system/registry classes, async I/O and cancellation need a
  real-process provider. Formatting/scanning coverage is limited, including wider
  scanf/locale cases. The harness's supported namespace is native paths and Z:.
- Process/thread lifetime, remote termination, remote context operations beyond
  the owned native-thread subset, TLS expansion/FLS, suspend/doorbell races and
  cross-process memory notifications need production contracts. Runtime side
  tables currently have harness/process lifetime.
- Signal conversion is qualified for the synchronous tested read fault and uses
  a standalone alternate stack. Calling frontend code from the signal handler
  needs an async-signal-safety/deferred-delivery audit before general process use.
- SMC/invalidation, KUSER_SHARED_DATA faults, x87/YMM recovery, adversarial vector
  nonvolatiles, PE32/guest-window mode and arbitrary-depth callback state remain
  unqualified. WoW64 has not executed.
- **Tier 0 retains 16 KiB pages and coarse 4 KiB protections.** The test faults a
  complete inaccessible host page. It does not prove distinct permissions or
  one-shot guards for four guest subpages in one host page. M2 needs an explicit
  compatibility policy; Tier 1/entitlement packaging is not counted as evidence.

**GO for an opt-in M2 engineering backend, with these limits made explicit and
qualification tests attached.** Actual translation, repaired faults and nested
entry are now demonstrated, so a FexProcess implementation has an executable base.
Default production use and parity claims remain NO-GO until the listed contracts
and representative application/performance tests pass. The current production
launch path and Rosetta behavior stay unchanged.
