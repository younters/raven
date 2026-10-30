# M1d merge review disposition

Branch `t3/fex-modhost-execution`, rebased onto macos `24430bf`.
All findings were checked against source and the running standalone harness.
No production launch, graphics or Rosetta changes were made. Nothing was pushed.

## Merge review

| Finding | Disposition | Regression/evidence |
| --- | --- | --- |
| 1. Stale macos base/protocol/tests | Fixed: replayed the ten FEX commits onto `24430bf`, no conflicts. | Base ancestry checked; entire diff restricted to FEX, scripts and Darwin CMake seam. `guest/protocol.h` and `tests` have zero diff against base. |
| 2. Unconditional build seam | Fixed: explicit `RAVEN_FEX_MODHOST` defaults OFF below header. | Standalone OFF configuration has zero tests; ON builds only area targets. |
| 3. Machine paths | Fixed: cache variable or environment for CMake; explicit CLI root or environment for scripts. | Missing root build-driver rejection and missing DLL skip tests. No hard-coded home-directory defaults. |
| 4. Invisible skips/substrate gating | Fixed: native tests always registered when option ON, execution remains registered with skip 77. | Missing-root CTest: three pass, execution visibly skipped with provisioning message. Provisioned: four pass. |
| 5. Unchecked claims | Fixed: nonzero DllMain required and attach marker enforced; exactly 127 verified auxiliary writes; pinned native CHPE target/RVA/instruction; module/patch SHA checks; exact 27-byte integer kernel and recomputed modulo-2^64 reference. | Live execution asserts all contracts; portable contract suite rejects changed DLL bytes and changed/missing/extra patches, checks header/pin RVA agreement. |
| 6. ABI sentinels | Fixed: EC full q8-q15/x18 and native d8-d15/x18 sentinels, native upper-vector clobber fixture. | Sixteen-argument call verifies 1496; sentinels checked before return; live signal test checks native and interrupted EC x18. |
| 7. Negative tests | Fixed: write/wrong-RIP/wrong-address repair rejection; unsupported unwind rejected; second fault in same host page is an exact expected failure. | Context and repair count stay unchanged on rejected faults; unwind child must SIGABRT with STATUS_NOT_SUPPORTED; gap runner rejects unrelated error/crash/timeout rather than hiding it with WILL_FAIL. |
| 8. Evidence/benchmark | Fixed: generator collects checked live evidence; original Rosetta source/log retained and hashed; full/warm medians and corrected ratios computed; build ids, fresh/cached label and --clean added. | Generator runs Release -j8, locked CTest, driver, artifact/license verification; seven portable contract tests include sample-order/median checks. |
| 9. License/lock documentation | Fixed: recipe inventory hash-verified and copied to external root/licenses; lock is explicitly an external dependency. | Generator verifies every copied license against inventory. |
| 10. Style | Pending the separate `style:` commit after functional checks. | Native harness C/C++/assembly will use tabs and K&R braces. Python and rebasable FEX patches retain their native styles. |

## Astra correctness review

| Finding | Verification and disposition | Regression |
| --- | --- | --- |
| A1. x18 leaks TEB into native code | Confirmed: old adapter's native observer saw TEB, native caller saw zero. Fixed: save native x18 in owned TEB extension, load it before native calls, restore EC TEB afterward. Signal assembly entry clears live x18 before C++. | Native d/vector/x18 fixture, live signal interrupted inside EC spin, every host callback's native-x18 guard, nested guest callback and native worker entry/exit. Tested profile requires native x18=0 under legacy-SDK mode; public custom-mode API remains unqualified. |
| A2. Dynamic imports unclassified/uncached | Confirmed: old resolve allocated adapters after one-time classification. Fixed: symbol cache under mutex and immediate atomic bitmap mark. | Repeated LdrGetProcedureAddress gives stable classified strlen pointer, called through real EC checker; eight threads resolve same symbol concurrently. |
| A3. Missing x87 status/tag | Confirmed: old StoreRavenContext omitted both fields subsequently loaded by FEX. Fixed in isolated MIT frontend patch 7. | Guest with occupied x87 stack, TOP=6 and C0 set crosses dispatch and repaired fault, computes exact 1+pi, ends with empty stack. |
| A4. Private section views | Confirmed: all old views MAP_PRIVATE, anonymous views lacked shared backing. Fixed: unlinked shared fd backing; MAP_SHARED for ordinary sections, MAP_PRIVATE for WRITECOPY. | Two views share writes for anonymous and real-file sections; third COW view stays private; file readback sees shared write. |
| A5. PAGE_EXECUTE_READ drops EXEC | Mapping bug confirmed; the exact claim that old protection succeeded then faulted was not reproduced here: Darwin rejects re-protection of MAP_JIT outright. Fixed EC 0x20 mapping plus honest Tier 0 JIT RX policy; unsupported per-region transitions fail. | Execute RET after native EC 0x20 transition; guest 0x20 remains data-only; request during writable JIT scope fails without changing old-protection output. Per-region guard limitation documented in M1D. |
| A6. Concurrent JIT vector writes | Confirmed: both allocators appended without synchronization. Fixed: locked NativeJit tracking and cached adapter construction. | Eight threads resolve same cached symbol and concurrently create/execute 256 immutable mappings; all regions retained and counted. |
| A7. Self-termination ownership/longjmp | Confirmed: old local shared_ptr destructor skipped. Fixed: ownership scope ends before jump; caller-owned frame remains to clean worker state normally. | 64 self-terminating workers return status 37; weak control references expire, environment live count and handle count return to baseline. |

## Risks carried into M2

Alternate signal stacks are now installed on every owned worker (native worker
checks that its stack is enabled). This cheap part is fixed. Synchronous signal
calls into FEX and callbacks are **not established async-signal-safe**. Secondary
workers still have no FEX ThreadInit/ThreadTerm and cannot claim translated worker
execution. Arbitrary signal/nonlocal-exit JIT write-mode restoration remains
unqualified. See [M1D.md](M1D.md) for these risks, native MAP_JIT per-region guard
limits, the visible Tier 0 4K/16K gap, remaining NT surface and the bounded M2 GO.
