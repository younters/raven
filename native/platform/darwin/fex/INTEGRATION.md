# macOS FEX integration

The default Apple arm64 build includes FEX. The modern SDK `raven` redirects
explicit FEX launches to the macOS 11 SDK companion `raven-fex-host`; native
launches retain Rosetta. Children select the modern sibling executable and
inherit FEX for both PE64 and PE32 (WoW64). Prefix setup follows the selected
engine too.

The reviewed default build, M5, Rosetta continuation regression, and M4 branches
are merged in that order on `t3/fex-integration`. M5 supplies the sole
`enter_nt_service`: it validates stopped state, pending syscall identity and the
readable/executable syscall/ret boundary, transfers R10 to RCX, and enters API
return state. M4's redundant definition is removed. Preferred-image reservations
apply only to x64; WoW64 owns a relocated window instead.

## Host ABI v2 and private TLS

Patch 0018 extends the common table to `RavenHostV2`, version 2, 104 bytes,
with an explicit `private_teb()` native callback. The WoW64 wrapper remains
version 1 and is now 120 bytes, embedding the common v2 table. Both frontends
reject old versions, old sizes and a missing callback before accepting the host.
The runtime probes old-version and truncated tables before valid registration;
the ABI mismatch tests require those rejections.

`private_teb()` returns the current host worker's preallocated
`RavenNT::ThreadEnvironment::teb`. The cooperative owner and each parallel
resident worker have separate ThreadEnvironment storage initialized before
module ThreadInit. Worker ThreadTerm runs before worker storage is destroyed.
When `active_thread` is absent, the callback returns fixed per-pthread fallback
storage with separate TlsSlots and a stable allocator identity. It
allocates nothing and takes no locks, including during module fault repair.
The cooperative/process owner calls module ThreadTerm and then releases its private environment when FexProcess is destroyed.

The shared module WinAPI helper `GetCurrentTEB()` uses this callback for its
internal TLS, PEB TLS bitmap, last-error and host thread identity. Both ARM64EC
and WoW64 compile these shared WinAPI sources. FLS helpers are unimplemented
and do not access guest slots. Execution frontend uses of `NtCurrentTeb()` and
x18 continue to refer to the execution TEB; x64 guest GS and WoW64 guest FS
continue to identify the selected guest thread.

The upstream change `8889a53` reserves guest TLS slot zero. That exposed a
pre-existing namespace collision: FEX's internal WinAPI TlsAlloc also starts at
one and stored emulator state in the execution guest's `TlsSlots[1]`. A guest
reader therefore saw a FEX pointer in its first allocated slot. Diagnostics
confirmed cached GS matched the selected guest TEB; refreshing segments did
not fix the collision and that attempted change was removed. No guest slots
are stolen or reserved by this fix. A dedicated fixture allocates every guest
slot 1–1087, covers all available inline slots and expansion slots, checks
exhaustion, and checks parent/child thread isolation under both engines and
both guest widths, cooperative and parallel.

## Private modules

The series is 0001–0019. Patch 0017 is regenerated against M4's final 0016 and
keeps alignment/overcommit repair after native-PC boundary validation. Patch
0018 records the ABI/TLS change independently of reviewed history. Modules are
built only in private roots (current: `/tmp/raven-review-modules/FEX-darwin`), from upstream source
`370c63057fc9fc826b58fe15d59930876df55b5f`; shared component trees are untouched.
The pinned module digests and entry instructions are in
`scripts/fex-modhost-pins.json` and `modhost/module_contract.h`.

Reproduction follows M5.md using a fresh private root and build id. Final
qualification results and application screenshots are recorded separately.

The upstream shared-clock descriptor special case also maps a WoW64 window view; otherwise initialization writes through an unmapped relocated guest address.

## Allocator identity and fixture setup

Module-private TLS also requires a matching allocator ownership identity.
Rpmalloc's Windows `get_thread_id()` used raw NtCurrentTeb, so after TLS moved
to private storage its owner comparisons still used changing guest TEBs. This
sent same-worker frees into its cross-thread freelist and hung native allocator
CAS retries during windowscodecs registration. Patch 0018 force-includes a
Raven-only header in the rpmalloc translation unit, routing that identity query
to `RavenPrivateTEB()` and the explicit host callback. The pinned rpmalloc
submodule itself remains unmodified. Separate workers still have distinct
identities and real cross-worker frees retain the existing remote-free path.
Both frontend ThreadTerm paths call DeinitCRTThread, which calls
rpmalloc_thread_finalize, before the private host environment is released.
The other direct NtCurrentTeb uses are execution CPU-area/GS/exception state
or process initialization (including the CRT's initial environment snapshot),
where entry uses the private host TEB before guest simulation.

Ordinary fixture runs use `RAVEN_PREFIX_SETUP=1`. They test execution contracts
with short deadlines and should not repeatedly charge all eight COM registration
processes against a tiny fixture's outer timeout. Four normal CTest registration
checks explicitly unset that variable and run serially, for both engines and
both target widths; they require the registration stamp and report elapsed
setup-plus-fixture time. Prefix preparation registers both component widths in
each case. Notepad++ also uses real fresh setup under both engines.

Final verification uses the provisioned RTL at
`/Users/yount/Documents/Projects/raven-components/rtl/raven-rtl.dll`, selected
through `RAVEN_RTL_COMPONENT_PATH`. Earlier diagnostics used a private RTL;
those inputs are historical. Shared component trees are never modified.
