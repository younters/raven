# DXMT for the Darwin guest

Source: https://github.com/3Shain/dxmt

Pin: v0.80, commit `589adb780354b461645b29999cefaf533594ee99`.

Release: https://github.com/3Shain/dxmt/releases/download/v0.80/dxmt-v0.80-builtin.tar.gz

Archive size: 18,681,669 bytes. SHA-256: `8f260e36b5739e68f3bad613381441385c4dc7b85b78ba8de653d5a6a264529d`.

`scripts/provision-dxmt.py` downloads that archive outside the source tree. It installs only its four x86_64 PE graphics libraries
and its x86_64 Mach-O `winemetal.so`. It builds Raven's own adapter and rewrites the two Wine dylib dependencies of the unixlib
to that adapter, then ad-hoc signs the changed Mach-O. The installed manifest records both the archive pin and each resulting file
hash. No DXMT implementation source is copied into Raven. Raven's adapter is an altered environment, not an upstream DXMT build.

The v0.80 root `LICENSE` is MIT (`DXMT.LICENSE`), copyright Feifan He. The release notes identify v0.80 as the last MIT release;
later releases use LGPL. Its DXVK-derived utility code retains the zlib/libpng notice (`src/util/dxvk.LICENSE`, `DXVK.LICENSE` here).
Microsoft DXBC parser headers carry MIT notices. Keep their notices when rebuilding or distributing source.

The release's `.github/workflows/ci.yml` pins LLVM `llvmorg-15.0.7`, Wine tools/runtime archives `v8.16-3shain` from
https://github.com/3Shain/wine/releases/tag/v8.16-3shain, and LLVM-MinGW `20251216`. LLVM's Apache-2.0-with-exception and bundled
third-party notices are in `LLVM.LICENSE`; Wine's LGPL-2.1-or-later text is in `WINE.LICENSE`. Wine CRT support is linked into
the PE DLLs. The full release build inputs and corresponding source, not just these notices, must accompany any redistribution
that requires them. This provisioner is for private local use and does not redistribute the binary package.

Wine ABI references (read for interoperability; Raven's gate is independently implemented):

- https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/winecrt0/unix_lib.c
- https://github.com/wine-mirror/wine/blob/wine-10.0/include/winternl.h (`MemoryWineUnixFuncs = 1000`, WoW64 = 1001)
- https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/unix/virtual.c

The host-side implementation and limitations are described in `native/platform/darwin/DXMT.md`.
