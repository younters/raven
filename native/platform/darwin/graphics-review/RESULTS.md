# Graphics unix-call review, 2026-10-31

Branch: `impl/graphics-unix-review`. Rebased onto local `macos`
`b144eb3bdd16d6fee05e0c35f30ff895899787d9`, as requested. This replaces the
initial partial report. Implementation commits after the rebase are
`883e4834`, `80a5b3de`, and `600b55cf`, in addition to the earlier review fixes.

## Disposition

1. **Implemented and qualified for the existing engine/backend combinations.**
   Both FEX gates and the Rosetta agent require a compiled contract with the full
   argument size for each ordinal before dispatch. The WoW64 stack words and
   complete argument range are checked before translation. mtld3d `host_in()`
   checks nulls, arithmetic, the 32-bit window and the complete byte extent;
   typed `host()` additionally checks multiplication and access permissions.
   Its 22-call contract preflights nested fields, and the native readers check
   subsequent arrays, packet payloads, tokens, metadata and output ranges.
   DXMT/Mullion provisioning instruments the pinned native readers, including
   command payloads, shader arguments, strings, arrays and reflection copies.
   Provisioning rejects unreviewed source hashes and unclassified pointer fields
   or argument blocks. FEX checks logical 4 KiB permissions as well as Darwin
   mappings, so a 16 KiB host page cannot hide a guest PAGE_NOACCESS boundary.

   Per-thread `sigsetjmp` recovery surrounds validation and synchronous native
   dispatch. SIGSEGV/SIGBUS returns `STATUS_ACCESS_VIOLATION`; FEX's existing
   JIT/overcommit/alignment handling runs first. mtld3d no longer installs a
   competing crash handler during logger initialization. Negative tests cover
   bad top-level pointers, partial argument blocks, bad nested pointers, integer
   overflow, physical and logical PROT_NONE crossings, repeated native faults,
   and a successful call after failure. The real mtld3d test also forces a
   native object fault after logger initialization.

   Qualification covers DXMT/Mullion PE64 under **Rosetta and FEX**, and mtld3d
   **PE32 and PE64 under FEX**. Synthetic dispatch tests cover Rosetta PE64 and
   FEX PE32/PE64. This does not introduce a Rosetta mtld3d port or a WoW64
   DXMT/Mullion table. The recovery frame is synchronous: it is not a lifetime
   lease for asynchronous work, nor does longjmp unwind native locks or roll
   back side effects. Checks at native readers remain the primary protection;
   arbitrary native-object corruption or concurrent guest unmapping is not
   established safe by these tests.

2. **Fixed / existing behavior verified.** Exact backend counts are DXMT
   132/146, Mullion 197, and mtld3d 22. DXMT's exact allowlist was already present;
   the relaxed Mullion check was corrected. Mullion wrappers require selected
   backend as well as count. Each provisioned library exports a versioned
   contract whose count is compiled from the actual table array length, rather
   than the sidecar. Both loaders compare that count before table access.
   A forged sidecar/count fixture is rejected, and a subsequent valid load in
   the same process succeeds. FEX already keyed handles by path and rejected
   changed counts; Rosetta already returned its agent's real handles. Missing
   overlay diagnostics now include the missing library's full path. Older
   overlays lacking the compiled contract must be reprovisioned.

3. **Installed defect not reproduced; builders fixed and verified.** The
   originally specified shared arm64 winemetal.so already used
   `@loader_path/raven-dxmt.dylib`. `rewrite()` now preserves install-name tokens;
   builders verify loader-relative output and derive counts from the parsed or
   compiled table. All four DXMT/Mullion translators and adapters were rebuilt
   privately for the checked readers; shared components were not changed.

4. **Fixed.** The adapter uses weak buffer records and takes a strong snapshot
   during readback. External releases invalidate weak records. No original
   retain/release callback runs under `buffers_lock`. The registry fixture
   checks external release and range overflow without requiring a GPU.

5. **Fixed.** Adapters use RTLD_LOCAL. DXMT/Mullion bind presentation callbacks
   explicitly; NtSetEvent remains a two-level library dependency. mtld3d uses a
   versioned callback structure and removes dynamic symbol lookup from its build.
   Load failures release temporary dlopen references; callback publication occurs
   only after validation. Tests verify that NtSetEvent and mtld3d callback symbols
   do not enter RTLD_DEFAULT, and a failed contract load permits a valid retry.

6. **Fixed.** All ten macdrv function offsets, four window-data fields and eight
   layer-property fields have size/offset assertions in both adapter and compiled
   native consumers. Callback and unix-contract structures have matching C/Rust
   ABI assertions. `mac_main` handles null, checks the block header, descriptor,
   capture extent and executable callbacks, then copies the block for dispatch.
   Tests exercise valid captures and malformed block/descriptor/invoke pointers.
   arm64 slots 72/73 use Raven's create/release wrappers.

7. **Fixed / alleged conversion defects narrowed.** Aligned allocation rejects
   arithmetic overflow with ENOMEM; realloc uses the same rounded offset header
   as free. `wcrtomb_s` converts into temporary storage and checks capacity before
   copying. The existing C-locale `wcrtomb` already produced only one byte and set
   EILSEQ, so those alleged behaviors were not reproduced. The now-built locale
   fixture runs under both engines with non-ASCII, errno, capacity, overflow and
   offset-reallocation cases. Qualification selects the private CRT.

8. **Fixed.** Commit-only aligned allocation with null base takes the aligned
   reservation path; a nonzero base with an alignment requirement is rejected.
   Both-width fixtures check alignment and writable memory. See the
   [VirtualAlloc2 contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2).

9. **Validation reordered.** GetCursorInfo validates output before reading
   cbSize. The old `call.load` already used checked process.read_memory; an
   unchecked native dereference was not established by that finding.

10. **Fixed / verified.** RAVEN_TRACE_UNIX is cached. Raven's build depends on
    Mullion fixture targets. Selecting Mullion without a root already reports
    an explicit error. The runner requires load confirmation for the selected
    d3d12, dxgi and winemetal DLLs. Backend selection is forwarded through the
    Rosetta agent's sanitized environment.

## Build and artifacts

RelWithDebInfo Ninja build: `/tmp/raven-graphics-review-build`, `-j8`; fixture
targets built explicitly. Requested cache paths were copied from build-macos,
then graphics and controls roots were replaced with private qualification roots:

- DXMT: `/tmp/raven-graphics-v4-overlays/dxmt`, including `unix64` and `unix-arm64`.
- Mullion: `/tmp/raven-graphics-v4-overlays/mullion`, including both unix arches.
- mtld3d: `/tmp/raven-graphics-v2-mtld3d` (also the final D3D9 test environment root).
- Controls: `/tmp/raven-graphics-review-controls`; PE32 CRT built in the test build.

The overlay directory names are historical; [private-hashes.json](private-hashes.json)
records the **final tested bytes**, including rebuilt translators/adapters and
unchanged copied mtld3d PE DLLs. Source provisioning is reproducible through
`dxmt/build_checked.py`, `dxmt/build_arm64.py`, the pinned source hashes, and
`mtld3d/build_arm64.py` plus its checked-in patch. Private native build sources
are under `/tmp/raven-graphics-v5-{dxmt,mullion}-{arm64,x86_64}` and
`/tmp/raven-graphics-v2-mtld3d/src`. Final adapters are also staged under
`/tmp/raven-graphics-v6-adapters`.

Shared FEX modules were used **read-only**, with no additional FEX patches:

- Root: `/Users/yount/Documents/Projects/raven-components/fex-modules`.
- xtajit64.dll: `fc6e04e4065da6559080c1f0189e69d50fa050c5e9d9cbb56d2c808f045817cd`.
- xtajit.dll: `a4dcb4a01c3ea03b360a71c8412c31fafb404bd5ae83142844e6a9f067695e70`.

No push, debugger attachment, shared-component modification or process kill was
performed. Observed free disk space remained above 8 GiB (15 GiB minimum).

## Qualification

The final focused selection `dxmt|mtld3d|mullion|crt|fex|unix` passed
**536/536** in 216.24 seconds, including cleanup fixtures.
The final full suite (v8) passed **1,564**, skipped **six**, and disabled
**one**, with **zero failures** out of 1,571 registered tests (189.36 seconds).
Final run results are recorded in [tests.json](tests.json). All ctest invocations
used `/tmp/raven-ctest-lock.sh`. Full suites set `RAVEN_PREFIX_SETUP=1`; D3D9
uses the private rebuilt mtld3d root above. Logs and JUnit files are under
`/tmp/raven-graphics-v7-focused.{log,xml}` and
`/tmp/raven-graphics-v8-full.{log,xml}`.

The earlier v6 full suite passed 1,564 tests, skipped six and disabled one
(1,571 registered). Its focused run had two initialization failures caused by
host ASLR occupying the negative fixture's default PE64 address, before graphics
ran. Commit `600b55cf` places that fixture outside the randomized host-image
range and reserves it before host allocations. The v6 negative-only run passed
20/20, including cleanup tests. The final mtld3d Rust translation test passed
1/1 (`mtld3d-shared`, release profile); an initial command against `mtld3d-unix`
selected zero tests and is not counted as validation.

Capability skips include Vulkan descriptor-buffer fixtures, Minesweeper,
capture authorization, widl, and the low-hard-limit fd-budget fixture. No
screen-lock failure was observed. The capture test's authorization skip is
not evidence of successful window capture.

The v7 full run had one intermittent failure: `darwin_dxmt_fullscreen_fex`
returned normally but all four GPU readbacks contained only their expected
clear colors, without the triangle. It passed in the focused run, the v6 full
run, and five consecutive isolated repetitions with `RAVEN_PREFIX_SETUP=1`
(26.17 seconds, plus five cleanup repetitions). No code or assertion was
changed for the repeats. This is **not classified as a screen-lock failure**:
these are GPU readbacks, and the cause is unproven. Preserved evidence is in
`/tmp/raven-graphics-v7-fullscreen-failure`; the failed full run and successful
repetitions are retained in tests.json rather than hidden by the rerun.

The final v8 full rerun passed the fullscreen fixture as well as every other
enabled, non-skipped case. Its passing result does not establish the cause of
the earlier intermittent failure.
