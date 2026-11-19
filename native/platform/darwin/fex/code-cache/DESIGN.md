# FEX cross-process code cache (Tier 0)

Opt-in: `RAVEN_FEX_CODE_CACHE=1` with `--execution=fex`, x64 (xtajit64.dll) only.
WoW64 never enables it. Off by default until the measurements below justify it.

## Reuse, not invention

The pinned FEX-darwin tree already carries upstream's per-block **DiskCache**
(FEX-Emu 7a82b07b…7962f400, backported with FormatVersion 0xA613). It stores
relocatable **host code per translated block**, plus entry points, packed
relocations and the guest bytes it was compiled from, in a pair of
append-only Fossilize files (`RWCacheDB_<bucket>.foz` and `_idx.foz`) that
are written under file locks by a low-priority writer thread. Lookup happens
in `CompileBlock` before the compiler runs; a hit copies the code into the
JIT buffer, applies relocations and registers the entry points as if the
block had just been compiled. Upstream's AOT/CodeMap path is not used: it
needs offline generation and its loader is not bounds-safe.

What Raven lacked was the frontend side: guest images were never registered,
so every block was "anonymous". Patch 0064 adds that, plus the hardening
below. The module host needed two fixes for FEX's file and thread use:
`NtLockFile` without a status block (FEX's `LockFileEx`, as on Wine) and
`NtCreateThreadEx` with skip-loader-init for FEX's writer thread, which also
gets the module's static TLS.

## What is cached

Host AArch64 code for one block (or multiblock), its entry-point table, its
relocations (guest RIPs relative to the block start, named FEX symbols,
patchable guest data) and a copy of the guest bytes. No IR. No per-process
pointers: every absolute host or guest address in the code is a relocation
and is patched on load (`CodeCache::ApplyPackedCodeRelocations`). Raven's
own JIT additions (0008 CheckDiv, 0015/0022 doorbell checks) address state
through the frame register only, so they relocate trivially.

## Key

- Directory: `~/Library/Caches/Raven/fex-code/<module-id>/`, where
  `<module-id>` is the first 16 hex digits of the SHA-256 of the loaded
  xtajit64.dll. Any patch, build option or toolchain change gets a fresh
  namespace. Override with `RAVEN_FEX_CODE_CACHE_DIR`.
- Inside it, FEX's buckets: machine = FormatVersion + host CPU-feature hash;
  process = 64-bit mode, host type (ARM64EC) and every config option marked
  `AffectsCodeGen` (TSO, multiblock, SMC mode, x87 precision, …).
- Per block: `XXH3(guest RIP - image base, FileId)` mixed with the bucket
  and the writable/TF/mono flags. **FileId** is XXH3-64 over the image's
  SizeOfImage, TimeDateStamp and the raw bytes of every executable,
  non-writable section, computed in the process when the image is
  registered. Same DLL contents at the same base gives the same FileId;
  different bytes under the same name never share a key.
- Collisions are harmless: every hit is still validated against live bytes.

## Validation (what never runs from the cache)

1. Only ranges registered by Raven's `map_image` qualify: executable,
   non-writable PE sections of a file-backed image (`RavenCodeCache` op 1).
   Anonymous memory, heaps and JIT buffers (V8) have no section and are
   never looked up or stored; anonymous caching is compiled out.
2. A section is dropped from the cache for the rest of the process as soon
   as any byte of it may change: protection made writable, a Raven
   `write_memory` (cross-process write) or a ranged instruction-cache flush.
   Freed or unmapped images are unregistered.
3. FEX's range query must report the page as non-writable and not shared
   (0061 RWX tracking, 0063 shared views); otherwise no lookup, no store.
4. On every hit the live guest bytes covered by the block's decoded extents
   are hashed (XXH3-128) and must equal the stored hash. A page whose bytes
   differ, at any offset the block was decoded from, never runs cached code.
5. Integrity: each index record carries an XXH3-64 of the stored blob
   (header, host code, entry points, relocations), seeded with the lookup
   key. A mismatch, short read, out-of-range offset or bad size rejects the
   entry and the block is compiled normally. Without this, a truncated main
   file followed by new appends would let stale index records point into
   new data.

## Storage, concurrency, eviction

- Writers append blob then index record, each under an exclusive lock
  (`NtLockFile` → `fcntl`) on a sentinel byte; the file size is read under
  the lock, so concurrent processes never interleave records. Readers build
  their index once at start-up, holding the same locks so they see whole
  records; entries written later by other processes are picked up by the
  next process (Chrome children start after the browser has written).
- A writer killed mid-append (Chrome and the harness kill processes
  freely) can leave a torn last index record. That is not damage: blobs are
  written before their record, so every whole record is valid. Readers stop
  at the torn record; the next writer, under the locks, steps over records
  appended since it last looked and trims the torn bytes before appending,
  so later records stay reachable.
- Real corruption found at start-up (a whole record pointing outside the
  main file, impossible sizes or extents) marks the database damaged: the
  process keeps using records that pass validation but writes nothing, and
  Raven renames the directory aside so the next process starts clean. A
  database whose header FEX rejects is set aside the same way.
- Size cap: `RAVEN_FEX_CODE_CACHE_MAX_MB` (default 2048; one cold Chrome
  launch writes about 0.8 GB). FEX stops appending when the main file
  reaches 31/32 of the cap. At start-up Raven evicts a namespace that has
  reached that size, so a full cache is refilled instead of never caching
  new code again, and removes sibling namespaces (old module builds) unused
  for 14 days. Whole-namespace eviction keeps the append-only format and
  needs no compaction; a process still using renamed files keeps its
  descriptors.
- Cache files are mapped read-only by FEX only through positional reads
  (`DiskCacheFileMapping=0`), so no large host mapping lands in the guest's
  address space.

## Costs

- While a process can write the cache, FEX emits relocatable (NOP-padded)
  constant loads in all code it compiles, including code it never stores.
- Each process hashes its images' executable sections once at load (about
  30 ms for Chrome's set) and reads the index once at start (about 25 ms
  for a 125k-record index).
- Processes that start together compile and store the same blocks; the
  duplicates (about a quarter of a cold Chrome launch) are harmless but
  take space.

## Tier 0 notes

16 KiB host pages do not matter: keys and validation use guest 4 KiB
offsets and exact byte extents. Software TSO and every other codegen
option are in the process bucket. Nothing needs the cross-arch
entitlement: cached code is copied into the existing MAP_JIT buffer.

## Measuring

`RAVEN_FEX_CODE_CACHE_STATS=1` prints per-process hits, misses, stores and
rejections at normal exit. Results are in `RESULTS.md`.
