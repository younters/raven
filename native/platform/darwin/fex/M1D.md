# M1d: native FEX module execution

The standalone arm64 harness binds all 127 imports and executes native DllMain,
Raven process/thread init, a returning x86-64 dispatch, precise read-AV repair,
a nested callback, occupied x87 state, and verified compute/round-trip kernels.
The review regressions also pass. This is **GO for an opt-in M2 engineering
backend** (`FexProcess` behind `--execution=fex`), with the remaining qualification
work below. Default production use remains NO-GO. Production launch, Rosetta and
graphics code are unchanged; nothing was pushed.

Branch: `t3/fex-modhost-execution`. Rebased onto macos `24430bf` without conflicts.
Only `native/platform/darwin/fex/**`, `scripts/**` and the Darwin CMake seam differ
from that base. Review dispositions and regression coverage are in [REVIEW.md](REVIEW.md).
Earlier evidence files retain historical partial checkpoints; their timings and
artifact identities do not describe the current seven-patch build.

## Reproduction and evidence

Set the external module directory explicitly; no script or CMake configuration
supplies a developer's home directory. Python 3, CMake, Xcode's native tools and
**the externally supplied `/tmp/raven-ctest-lock.sh`** are prerequisites. The lock
script is not bundled or created by Raven. Module provisioning additionally uses
the pinned llvm-mingw toolchain and FEX-darwin recipe.

```sh
export RAVEN_FEX_MODULE_ROOT=/absolute/path/to/fex-modules
python3 scripts/build-fex-modules.py --raven-host --build-only --build-id m1d-review-x87
# A distinct id starts a fresh recipe build. --clean removes ONLY that id's output.
# python3 scripts/build-fex-modules.py --raven-host --build-only --build-id my-fresh-build --clean
python3 scripts/test-fex-modhost.py --evidence /tmp/fex-runtime.json
python3 scripts/generate-fex-modhost-evidence.py
```

The generator recreates [modhost/m1d-runtime-evidence.json](modhost/m1d-runtime-evidence.json)
by configuring Release, building **-j8**, running locked CTest, then running the
checked guest driver. It retains commands, return codes, stdout/stderr, source and
script hashes, artifact/patch identities, license hashes and all samples. It
rejects an incomplete, failed or skipped provisioned suite. Reproducible means
reproducible collection and calculations; timestamps, addresses and timings vary.
The current evidence records a cached module build explicitly. The named
`m1d-review-x87` recipe was built fresh after patch 7; its first wrapper run reached
native self-test and exposed the MAP_JIT protection limit. The later cached wrapper
completed build/audit and native checks successfully. Cached output is never
presented as a fresh build.

Standalone area tests, without building Raven or unrelated fixtures:

```sh
cmake -S native/platform/darwin/fex/modhost -B /tmp/raven-m1d-cmake \
  -DRAVEN_FEX_MODHOST=ON -DCMAKE_BUILD_TYPE=Release \
  -DRAVEN_FEX_MODULE_ROOT="$RAVEN_FEX_MODULE_ROOT"
cmake --build /tmp/raven-m1d-cmake -j8
/tmp/raven-ctest-lock.sh fex-m1d ctest --test-dir /tmp/raven-m1d-cmake --output-on-failure
```

`RAVEN_FEX_MODHOST` defaults OFF, including in Raven's normal Darwin build. With
it ON, the substrate, ABI, driver-contract and Tier 0 gap tests need no DLL.
Execution remains registered and visibly exits **77 / SKIP** when the DLL is
missing (`SKIP_RETURN_CODE 77`). Release assertions remain enabled. The provisioned
suite passes **4/4**, including the exact expected-failure gap probe; that probe
passes CTest only when the known gap is observed. An unrelated crash or unexpected
result fails it. The missing-module suite has three passes and one visible skip;
the OFF configuration has no tests or harness targets.

Pins: FEX-darwin `370c63057fc9fc826b58fe15d59930876df55b5f`, llvm-mingw
`20260616-ucrt-macos-universal`. The execution driver enforces the version-controlled
module and seven patch SHA256s in [scripts/fex-modhost-pins.json](../../../../scripts/fex-modhost-pins.json).
The current xtajit64 SHA256 is
`a02b05c884a028bb4acc1ba7701b0b64c7d3e26e226392bef4692ed7126bb7cb`.
WoW64 is built/audited but has not executed.

This runs on the same M4 Pro/macOS 27.2 host with 16 KiB pages and the legacy-SDK
linker flag. No entitlement, signing-policy, boot-argument or system-setting change
was used. Only the standalone arm64 harness was debugged. x86 assembly is compiled
as data for byte comparison; this fix round launched no x86 or Raven executable.

## Checked contracts

| Contract | Required result |
| --- | --- |
| Import binding | Exactly 127 resolved slots AND 127 verified auxiliary IAT writes |
| Native entry | Nonzero DllMain and native CHPE-selected RavenProcessInit at pinned RVA `0x5c04`, EC classification and first instruction checked |
| Initialization | Explicit successful DllMain attach, RavenProcessInit, ThreadInit markers |
| Returning dispatch | Arguments 7, 11, 13, 17; native return 224; resumed result **4884**, exact final RIP and one business dispatch |
| Read AV | Exact guest RIP/address; skip three-byte load, set RAX=0x1234; resumed result **4665** |
| Negative AV | Write fault, wrong RIP and wrong address return NOT_SUPPORTED, change no context and increment no repair counter |
| Nested callback | Guest callback argument 41 returns 124; outer guest resumes with **5008** |
| x87 | TOP=6, occupied tag 0xc0 and condition C0 survive dispatch and repaired fault; resumed addition yields exact 1+pi and empty stack |
| ABI | Native d8-d15, EC full q8-q15 and x18 sentinels survive adapters; adversarial native upper-vector clobbers; sixteen argument result 1496 |
| Native x18 | Native callbacks/import fixture, worker entry/exit and signal handler observe zero; EC gets its TEB back, including after signal return |
| Dynamic lookup | Repeated LdrGetProcedureAddress returns one cached adapter; EC checked indirect call through it returns strlen=7 |
| Concurrency | Eight concurrent resolvers obtain stable classified adapters; 256 concurrent immutable allocations execute and remain tracked |
| Sections | Two RW views share writes for pagefile and real file sections; WRITECOPY stays private; file readback agrees |
| Native RX | EC PAGE_EXECUTE_READ retains execution under real JIT write protection; guest 0x20 stays read-only data; RX request during write scope explicitly fails |
| Thread lifetime | 64 self-terminating workers return status 37, expire control ownership, restore handle/environment counts; worker altstack installed |
| Unwind | Unsupported opcode aborts with explicit STATUS_NOT_SUPPORTED diagnostic |
| Tier 0 gap | First 4K repair exposes second logical 4K page in same 16K host page: exact expected failure retained |
| Compute/round trip | Five verified 10M-iteration samples and five verified 100,000-call samples |

The host constructs PEB/process parameters, TEB, TLS vector/slots/bitmap, AMD64
context, CHPE CPU area and EC bitmap. The module has no PE static TLS template;
its runtime TLS allocations use the real harness allocator. Dynamic imports are
cached per symbol and classified immediately using atomic bitmap updates. Both
native-JIT ownership vectors and import-adapter construction are locked.

x18 points to the TEB only in EC. Each entry saves the native value in a private
owned TEB extension; each native adapter restores it before calling C++/libSystem
and reinstalls the TEB afterward. The tested **legacy-SDK native profile requires
x18=0**, checked at native entry and callbacks. The signal assembly trampoline
clears live x18 before C++; the kernel-saved interrupted value remains in ucontext
and is restored to EC on signal return. This does not qualify Apple's public
custom-x18 mode-switch API or other native x18 profiles.

## Measurements

The two function bodies exactly match retained SPIKE assembly, assembled and
compared independently before execution. Integer length is asserted **27 bytes**,
float length **19 bytes**. The integer reference is recomputed from seed 7 for
10M steps `x = x * 1664525 + 1013904223` modulo 2^64, giving
`5292597211099207303`. Float reference `4295705.5919631328` is compared bitwise,
compiled with `-ffp-contract=off`.

Historical Rosetta inputs are now retained:

- [baseline source](modhost/baseline/rosetta_bench.c), SHA256 `d1336f1cece97d6a0df1e1755c2a1f22e74d6f63b44391aea21794205616a2ad`.
- [baseline log](modhost/baseline/rosetta-bench.txt), SHA256 `4248d5f5ec04d54796847f1e10784b3ea9fbc637339617038fbb12341ed825dd`.
- [kernel assembly](modhost/bench_kernels_x86_64.S), SHA256 `c9692eb5f85956694a2bee48df135df4c58bf2c526ac6b22558dffcc94e1dfaa`.

The retained log's full medians supersede the rounded SPIKE summary values of
9.293/12.525 ms. Warm medians exclude sample 0 for **both** engines. In the old
retained FEX evidence the integer full median happened to be sample 0; this is why
warm figures must be shown separately. In current runs the median's sample index
can differ. The generator calculates both directly from samples, without selecting
an advantageous sample.

<!-- measurements:start -->
| 10M iterations | Rosetta full / warm ms | FEX full / warm ms | Full FEX / full Rosetta | Warm FEX / full Rosetta |
| --- | ---: | ---: | ---: | ---: |
| Integer | 9.226208 / 9.211125 | 21.486292 / 21.087688 | 2.329× | 2.286× |
| Float | 12.277667 / 12.296855 | 25.555250 / 25.467750 | 2.081× | 2.074× |

Round-trip median: **121.854 ns/call** (all five), **122.775 ns/call** (warm samples 1–4). Raw samples are retained in the evidence JSON.
<!-- measurements:end -->

Sample 0 includes the benchmark's initial block compilation; samples 1–4 reuse
it. Timings cover entry through verified completion and include the wrapper's
call/return and terminal syscall. Round-trip includes loop, syscall state spill,
native dispatch, context application and dispatcher reentry; it is not an isolated
native call or EC fast-call latency. Rosetta was not rerun. This is a same-machine
historical comparison under concurrent development load, not a controlled
application benchmark. Hardware TSO alone is not shown to close the compute gap.

## Patch boundaries and remaining risks

All Raven adapters are original code; no Wine implementation was copied or linked.
FEX notices remain intact. The build recipe's license inventory is hash-verified
and **copied to `$RAVEN_FEX_MODULE_ROOT/licenses`** by Raven's wrapper.
Seven isolated patches apply to the pinned MIT snapshot:

1. Deterministic PE timestamps.
2. Raven-owned versioned host interface/frontend.
3. Raven build option.
4. Syscall spill/dispatch/reentry and allocator/memory hooks.
5. Native temporary buffer guard sizing at 16K (the existing Core allocation
   exception; no instruction lowering changes).
6. Precise fault entry and context-packet construction.
7. x87 StatusWord (TOP/condition/IE) and abridged TagWord storage in that packet.

Native code uses MAP_JIT and nested per-thread write scopes. Immutable veneers
use ordinary RX mappings and execute during FEX write scopes. On this host,
`mprotect`/`mach_vm_protect` cannot re-protect MAP_JIT mappings (EACCES /
KERN_PROTECTION_FAILURE), including requests for guards. Native 0x20 therefore
uses actual per-thread JIT RX mode and rejects the request while writable. This
is **not independent per-region protection during a writable scope**. Native
MAP_JIT guard/decommit transitions remain unsupported; FEX's native buffer guard
setup is not qualified by the execution tests. This is additional to the tested
coarse guest-page limitation. It needs an M2 policy, not a passing fake.

Remaining NT/process qualification:

- Native C++/SEH cleanup, complete packed partial prologs/epilogs and language
  handlers; unsupported unwind opcodes and cleanup handlers fail explicitly.
- Windows file sharing/path/drive rules, locale/code pages, broader object/system/
  registry classes, async I/O/cancellation and wider formatting/scanning.
- Real process lifetime, remote termination/context operations, TLS expansion/FLS,
  suspend/doorbell races and cross-process notifications. Supported worker resource
  cleanup is tested, but side tables still have harness/process lifetime.
- **Signal async-safety is not established.** The handler calls FEX and the host
  callback synchronously; these calls may allocate or take locks. Alternate stacks
  are now installed on each owned worker as well as the constructing thread, but
  this does not make synchronous signal delivery generally safe.
- **Secondary native workers have an empty CPU area and no FEX ThreadInit/ThreadTerm.**
  Worker tests exercise native adapters/lifetime, not translated execution on those
  workers. Real FEX thread lifecycle must be connected before M2 translates there.
- **JIT write-protect restoration under arbitrary signal reentry and nonlocal exits
  is unqualified.** Normal nested modes and tested read-fault/nonlocal completion
  pass; faults during emission, callbacks exiting write scopes and production
  unwinding need dedicated contracts. No general safety claim follows.
- SMC/invalidation, KUSER_SHARED_DATA faults, wider x87 exceptions/control state,
  YMM/FP-vector nesting, arbitrary callback depth and PE32/guest-window execution.
  The occupied x87 regression qualifies only its tested stack/condition/tag case.
- **Tier 0 retains 16K pages and coarse 4K protection.** The expected-failure test
  permanently records loss of a second fault inside one host page. Tier 1 or an
  entitlement is not counted as evidence for this gap.

The standalone implementation is a concrete base for M2 development. Default use,
Windows parity and representative application/performance qualification remain
NO-GO until these limits have explicit contracts and tests.
