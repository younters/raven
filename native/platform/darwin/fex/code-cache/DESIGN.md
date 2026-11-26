# FEX code cache

Set `RAVEN_FEX_CODE_CACHE=1` to reuse translated code across x64 FEX launches. The cache is off by default and is
unavailable for WoW64.

Raven registers executable, nonwritable sections of file-backed PE images with FEX's block cache. A cache entry stores
relocatable ARM64 code, the entry points and relocations, and information identifying the guest bytes. FEX checks the
live guest bytes before copying code into its JIT buffer and applying relocations. Writable and shared ranges are
ineligible. Writes, protection changes and instruction-cache flushes can invalidate sections.

FEX stores blobs and index records in append-only files, with file locks for readers and writers. Readers load the
index at startup. A rejected entry is translated again. Raven separates cache directories by module digest and
rotates full caches at startup. Integrity checks detect stale or damaged records. The cache directory is trusted
because its records contain executable code.

| Setting | Default or behavior |
|---|---|
| `RAVEN_FEX_CODE_CACHE_DIR` | Base directory, normally `~/Library/Caches/Raven/fex-code` |
| `RAVEN_FEX_CODE_CACHE_MAX_MB` | Per-module size target, 2048 MiB by default |
| `RAVEN_FEX_CODE_CACHE_STATS=1` | Print cache statistics on normal exit |

`native_code_cache_fex_*` covers reuse and code changes under cooperative and parallel execution.
