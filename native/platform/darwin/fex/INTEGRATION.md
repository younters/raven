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
The callback only reads the already initialized `active_thread` pointer; it
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

The series is 0001–0018. Patch 0017 is regenerated against M4's final 0016 and
keeps alignment/overcommit repair after native-PC boundary validation. Patch
0018 records the ABI/TLS change independently of reviewed history. Modules are
built only in `/tmp/raven-integ-modules/FEX-darwin`, from upstream source
`370c63057fc9fc826b58fe15d59930876df55b5f`; shared component trees are untouched.
The pinned module digests and entry instructions are in
`scripts/fex-modhost-pins.json` and `modhost/module_contract.h`.

Reproduction follows M5.md using a fresh private root and build id. Final
qualification results and application screenshots are recorded separately.

The upstream shared-clock descriptor special case also maps a WoW64 window view; otherwise initialization writes through an unmapped relocated guest address.
