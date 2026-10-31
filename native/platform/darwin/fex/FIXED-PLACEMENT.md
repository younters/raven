# Fixed main-image placement

The baseline at `10f7e381` intermittently lost `0x140000000` before the
ModuleSet preflight. With `RAVEN_FEX_TRACE_MEMORY=1`, an in-process
`mach_vm_region(VM_REGION_EXTENDED_INFO)` query reported:

```
FEX collision requested=140000000+8000 region=140000000+8000000 tag=12 protection=3 status=0 path=
```

Tag 12 is `VM_MEMORY_MALLOC_MEDIUM`: a 128 MiB host malloc region, not a
DXMT image or FEX JIT mapping. Both pool and geometry reproduced this.
No debugger was used.

`raven-fex-host` now has a 4 GiB zero-fill `RAVEN_PE` segment at link address
`0x110000000`. The kernel owns that address space before dyld initializers
or host malloc can use it. It occupies no file bytes and is not touched.
The executable retains arm64 PIE/ASLR and its macOS 11 SDK contract.
The current XNU arm64 slide is fewer than 16 MiB plus 0, 1, or 2 translation-table
twigs (32 MiB each on this 16 KiB-page system); even the largest slide
leaves `0x140000000` inside the reservation.

At the first companion launch step, before Cocoa startup, Raven registers the
whole pool with `PROT_NONE` and reads the main PE header with `pread`. It
claims the main image before normal loader allocation. Existing loader
preflight and FexProcess consume that claim as before. Initial DLLs and
later fixed guest allocations also claim nonoverlapping slices of this pool.
Freeing a slice restores an anonymous `PROT_NONE` reservation and removes its
claim; it does not expose the address to host allocators. Overlap checks and
pool ownership precede every replacement mapping.

Keeping the unused pool is necessary: an initial implementation released
unused slices, after which Cocoa presentation fixtures faulted executing
zero-filled memory inside the former segment. The retained-pool revision
passed window capture, pacing, pool, and geometry with signal tracing enabled.
Host mmap/dlopen/JIT allocations must not reuse pages still described by the
host's Mach-O segment metadata.

This guarantees conventional x64 main-image and DLL bases inside the pool
against pre-main host allocations on the supported Darwin arm64 layout.
Addresses outside it still require a successful early non-overwriting Mach
reservation; this is not a guarantee for every arbitrary 64-bit address.
The complete PE parser remains responsible for validation.
WoW64 continues to use a private, aligned 4 GiB host window; `0x400000` is a
guest offset and does not compete with a host mapping at that numeric address.

The two `native_fixed_default*_pressure_fex` fixtures explicitly disable
relocations. The harness checks the PE image base and empty relocation
directory. Before normal loading, the opt-in pressure hook touches 64 MiB
of malloc allocations and loads arm64 `winemetal.so`; the harness also
requires confirmation that the hook ran. The guest checks its actual image
base, overlapping-allocation rejection, and zeroed reuse after release. Their bases are `0x140000000`
and `0x400000` respectively.

## Prior art and upstream inspection

`git fetch origin` fetched mirkobrombin/raven main at `3cd4ca60`; that branch
does not contain this fork's Darwin FEX implementation.

- [Wine loader](https://github.com/wine-mirror/wine/blob/master/loader/main.c)
  uses zero-fill Mach-O reservations to exclude framework allocations before
  main. Its x86 non-PIE linker recipe cannot be used directly on arm64.
- [FEX allocator](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Utils/Allocator.cpp)
  reserves address-space gaps for host allocation and installs allocator hooks.
  That alone cannot protect Raven against allocations made before FEX starts.
- [XNU slide policy](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c)
  and [Mach-O loading](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/mach_loader.c)
  explain the arm64 PIE slide and kernel segment ownership.

No FEX frontend patch or component rebuild is required.

## Qualification environment

Built from `macos` HEAD `10f7e381` in a new worktree branch using
`RelWithDebInfo` and `-j8`. FONT_HASHES, PNG paths, and the requested
RAVEN component/DXMT/SQLite paths were copied from the main build's cache.
Fixture targets were built explicitly. Every ctest invocation used
`/tmp/raven-ctest-lock.sh`.

The initial build used the requested shared module root. During the repeat
runs, that installation was replaced with modules carrying the thread-context
patches; this checkout correctly rejected their changed pinned ABI/RVA contract.
Qualification therefore switched to `/tmp/raven-placement-pinned-modules`,
containing copies of the original qualified binaries, verified against
`scripts/fex-modhost-pins.json`:

- xtajit64.dll: `703ee6a07b7f612941447272cccad9a8487b8dfa4e1e5c54d434fa643a6504bf`
- xtajit.dll: `b84bca9f2b7e8797c46c51729149c4c8dfeb35b5b230960635f5493929df45d9`

No component installation or other worktree was changed. The interrupted
ABI-mismatch batches are excluded from placement qualification. The final
pressure, DXMT, FEX-wide, and full-suite runs use `RAVEN_PREFIX_SETUP=1`.

## Results

The final pinned-module qualification completed on 2026-10-31:

- Fixed x64 main at `0x140000000`, malloc/DXMT pressure: **50/50 passed**.
- Fixed WoW64 main at `0x400000`, malloc/DXMT pressure: **50/50 passed**.
- All 21 `darwin_dxmt_*_fex` fixtures: **20/20 each**, 420 executions, no
  failures or skips. Window capture passed; no locked-screen exception was needed.
- Every test with `fex` in its name: **206/206 passed**, no skips;
  **379/379** including automatically included fixture cleanup checks.
- Full suite with `RAVEN_PREFIX_SETUP=1`: **26 failures / 1433 entries**, versus
  **30 failures / 1429 entries** at baseline. Both runs had 1 disabled entry
  and 6 skipped entries. The two new regressions add two tests and two cleanup
  entries.

Five baseline failures disappeared: `darwin_dxmt_compute_fex`,
`darwin_dxmt_deferred_fex`, `darwin_dxmt_keyed_x64_fex`,
`native_fresh_prefix_x32_fex`, and `native_fresh_prefix_x64_fex`. Each baseline
failure reported the unavailable preferred-base error. All FEX tests also
passed within the final full suite.

The 25 remaining baseline failures are recorded by name and output in
`fixed-placement-results.json`. One additional full-suite failure was
`native_darwin_cocoa_input` ("posted A down missed its window"). It passed
**10/10 isolated reruns**, consistent with a transient input-focus failure;
this result is recorded separately rather than removed from the full-suite
failure count. No code was changed for that unrelated test.

The baseline repeat logs are `/tmp/placement-baseline-repeat.log`; the final
repeat XML files are `/tmp/placement-dxmt-qualified/run-01.xml` through
`run-20.xml`. Full logs/XML use `/tmp/placement-baseline-full.*` and
`/tmp/placement-full.*`; FEX-wide results use `/tmp/placement-all-fex.*`;
pressure results use `/tmp/placement-pressure-qualified.*`.

## Review hardening

Startup now checks the actual slid segment covers `0x140000000`, and an early
reservation failure is fatal with the address in the diagnostic. The pressure
hook is compiled only with `RAVEN_FEX_PLACEMENT_TEST_HOOKS=ON` (default OFF).
The x64 harness requires the companion's successful in-pool claim diagnostic.
The test-only `RAVEN_FEX_DISABLE_IMAGE_POOL=1` bypasses registration: the same
x64 fixture must fail early because the Mach-O reservation remains occupied.
The 32-bit test is named `native_fixed_default32_window_pressure_fex`; it tests
the relocated guest window, not the image pool. Both fixtures now free and reuse
four simultaneous 1 MiB slices across four rounds, checking every page is zero.

Context debug registers currently provide sanitized read-back only. Neither
engine arms guest hardware breakpoints. DR7 reserved/global-detect bits are
removed. This is a compatibility limitation, not breakpoint implementation.
FPCR writes from patch 0023 execute on the worker: x64 BeginSimulation calls
SyncThreadContext -> LoadStateFromECContext; RavenWow64Run marks the CPU area
dirty and LockJITContext calls LoadStateFromWowContext before ExecuteThread.
No additional FEX module patch is necessary for this path.
