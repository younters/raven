# Graphics unix-call review, 2026-10-31

Base: ac36a2f077f8c7b70faf85ae41bca6d30d4b04ef. Branch: impl/graphics-unix-review.

This is a **partial implementation**, not a completed pointer-safety qualification.
Do not treat these commits as closing the review's highest-priority finding.

## Disposition

1. **Open.** Confirmed: FEX checks only an eight-byte WoW64 header; x64 passes
   R8 directly; mtld3d's address helper is unchecked. No comprehensive argument
   schema, nested-range checks, worker-thread lifetime protocol, fault recovery,
   or bad-pointer fixtures were added. Rosetta has a native-fault reporting path
   in guest/agent.c, but that is not proof of safe recovery. A signal longjmp
   alone would bypass native locks/destructors and does not solve retained worker
   addresses. This requires further implementation.
2. **Partial.** DXMT already used exact 132/146 counts in this HEAD; only Mullion
   accepted 1..4096. Mullion now requires exactly 197, and adapter wrapping checks
   RAVEN_D3D12_BACKEND. FEX already caches by path and rejects a changed count
   (load_unix_library); NativeProcess/Rosetta already obtains a real table handle
   from the agent, rather than unconditionally returning 1. Missing FEX library
   files now report `overlay missing at <root>`. **Open:** independent validation
   of the real table length against the sidecar; broader loader/cache tests.
3. **Installed defect not reproduced; builder hardened.** `otool -L` on the
   specified shared arm64 winemetal.so reports @loader_path/raven-dxmt.dylib.
   rewrite() now preserves @-prefixed install names; the builder verifies the
   final dependency and writes the parsed table count. No translator rebuild was
   needed: the private overlays copy existing winemetal.so and rebuild adapters.
4. **Fixed.** A weak NSHashTable replaces manual retain counts. Readback takes a
   strong snapshot, copies within the object's current CPU range, then drains its
   private pool. External releases invalidate weak records. Original table retain
   and release functions are no longer wrapped or called under buffers_lock.
   A GPU-independent Objective-C test covers external release and range overflow.
5. **Partial.** FEX DXMT/Mullion adapters use RTLD_LOCAL. Presentation callbacks
   are explicitly installed in the wrapped table, bypassing Wine's global
   macdrv lookup; NtSetEvent is a two-level Mach-O dependency of winemetal.so.
   Failed FEX loads close temporary adapter/image references; readback callback
   publication occurs after wrapping succeeds. **Open:** mtld3d is built with
   dynamic_lookup and still needs an explicit symbol-binding ABI; it retains
   RTLD_GLOBAL. Retry fault-injection coverage is also outstanding.
6. **Partial.** Added macdrv size/view/main offset assertions matching the pinned
   consumer's ten function pointers; mac_main accepts null. arm64 slots 72/73
   now use Raven's create/release wrappers. **Open:** validating arbitrary native
   block pointers, full ABI/negative tests and stronger consumer-generated layout
   verification.
7. **Fixed/hardened.** Allocation arithmetic rejects overflow with ENOMEM;
   aligned realloc reads the same rounded header as aligned free. wcrtomb_s now
   converts into temporary storage before checking/copying. At this HEAD the old
   wcrtomb already emits only one byte and sets EILSEQ, so those two alleged
   conversion defects were not reproducible with its C-locale implementation.
   Added both-engine ctests and non-ASCII, errno, overflow and offset-reallocation
   checks. The private CRT is selected for qualification.
8. **Fixed.** Null-base commit-only aligned allocations follow the aligned
   reservation path and include MEM_RESERVE in the final fixed-address operation.
   Nonzero base plus nonzero address-requirement alignment is rejected, following
   Microsoft's VirtualAlloc2 contract. Both-width runtime fixtures cover the
   commit-only result and actual writable memory.
   Reference: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2
9. **Reordered validation.** GetCursorInfo validates output before reading cbSize.
   The original call.load already uses checked process.read_memory, so source
   inspection does not establish an unchecked host dereference here.
10. **Fixed/verified.** RAVEN_TRACE_UNIX is cached. Building raven depends on the
    Mullion fixture targets. mullion_root already explicitly throws when no root
    is configured. The Rosetta agent now receives backend selection through its sanitized native
    environment. The runner now requires load confirmation for d3d12, dxgi and
    winemetal from the selected root, in addition to its pixel assertions.

## Build and artifacts

RelWithDebInfo Ninja build: /tmp/raven-graphics-review-build, -j8. Initial cache
copied the requested settings, plus Skia/font paths, from build-macos. FEX enabled
with /Users/yount/Documents/Projects/raven-components/fex-modules. Qualification
then selected private controls and DXMT/Mullion overlays. Fixture targets were
built explicitly. The first build caught a PE32 SIZE_MAX declaration issue;
using a size_t maximum fixed it. No FEX patches or shared-component writes.

Private artifact SHA-256 values are in private-hashes.json. Adapter sources are
in these commits; translator binaries are copies of the provisioned artifacts.
No push, debugger attachment or process kill was performed. Observed disk free
space stayed above 8 GiB (17 GiB minimum observed during tests).

## Tests

All ctest invocations use /tmp/raven-ctest-lock.sh. The focused selection
`dxmt|mtld3d|mullion|crt|fex|unix` completed 478 tests: 468 passed, 10 failed.
The failures were two unbuilt context-group fixtures and eight Mullion cases
whose private adapters initially omitted mullion/unreachable.c. The targets
were then built and the private adapters relinked with that existing source.
Both-width mtld3d rendering/runtime tests, all 42 DXMT engine cases, both CRT
locale tests, buffer-registry and unix-call tests passed in the first run.
The first recheck passed the two context tests and all four FEX Mullion cases,
but failed the four Rosetta Mullion cases: the sanitized agent environment did
not forward RAVEN_D3D12_BACKEND. Commit e7673cc0 fixes that forwarding.

The full suite completed in 207.22 seconds: **1,491 passed, 4 failed, 6 skipped,
1 disabled** (1,502 registered). Its first four tests launched before the above
rebuild completed and reproduced those same Rosetta Mullion failures. The rest
of the full suite passed or explicitly skipped. This was not an all-green
final-code full-suite invocation; the final targeted Mullion recheck passed **16/16** (eight graphics cases across
both engines plus eight cleanup tests), in 60.85 seconds. Every initially failing
focused/full-suite case has a subsequent passing run. This does not establish
the unresolved bad-pointer/fault-containment contracts.
No screen-lock failure was observed. Capture authorization was skipped by its
existing test gate, along with other capability-dependent checks. tests.json
records individual failure, skip and disabled names.

Logs and JUnit files: /tmp/raven-graphics-review-{focused,recheck,full,final-mullion}.{log,xml}.
The full suite uses RAVEN_PREFIX_SETUP=1 and -j4; the focused run uses -j1.
D3D9 uses the requested shared RAVEN_MTLD3D_ARM64_ROOT without modification.

