# FEX on macOS

FEX is an experimental execution engine for Apple silicon. It translates x64 and 32-bit x86 Windows code in the arm64
Raven process. Raven still handles NT services, Windows thread scheduling and the desktop. The 32-bit path runs in a
relocated 4 GiB WoW64 address window.

## Build and run

FEX is opt-in: configure with `-DRAVEN_FEX_MODHOST=ON` to build Raven's FEX host and its tests. The FEX modules
require separate provisioning, and Rosetta remains the default execution engine. An existing CMake cache keeps its
previous `RAVEN_FEX_MODHOST` value.

From the Raven source directory, with the macOS build tools installed, run:

```sh
modules="$PWD/../raven-components/fex-modules"
python3 native/platform/darwin/fex/scripts/build-fex-modules.py --raven-host --root "$modules"
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAVEN_FEX_MODHOST=ON -DRAVEN_FEX_MODULE_ROOT="$modules"
cmake --build build-macos
build-macos/native/raven --run-process --execution=fex --desktop=macos --timeout-ms=600000 program.exe
```

The script fetches the [pinned third-party Darwin source and toolchain](../../../upstream/fex/README.md) and applies
Raven's patch series. It installs `xtajit64.dll`, `xtajit.dll` and their license notices in the module directory.
`--raven-host` selects the patches and ABI Raven needs. There is no Raven module release to download. Provisioning
and process tests check the resulting module hashes against [`fex-modhost-pins.json`](scripts/fex-modhost-pins.json). The runtime checks
module entry layout and host ABI compatibility.

A nonempty `RAVEN_FEX_MODULE_ROOT` environment variable overrides the configured path. Without it, Raven tries
the configured directory, then `fex-modules` and `../raven-components/fex-modules` relative to the executable's directory.
x64 needs `xtajit64.dll`. WoW64 also needs `xtajit.dll`.

Keep `raven-fex-host` beside `raven`. An explicit FEX launch re-executes into the companion before loading the desktop.
Its Mach-O declares SDK 11.0 so the kernel preserves the ARM64 `x18` register that the Windows module ABI uses for the
thread environment block (TEB). This depends on [XNU's legacy SDK behavior](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/machine_task.c).

## Host interface

The module host maps the FEX PE modules and supplies their native imports, JIT allocation and `RavenHostV2` callbacks.
The WoW64 frontend wraps the same host table. The private TEB callback gives module WinAPI helpers their own TLS and
last-error state. The execution TEB and the guest's x64 GS or x86 FS continue to describe the selected Windows thread.
Cooperative execution runs inline. Parallel execution uses resident pthreads that return guest stop state to Raven.

The host tracks Windows memory permissions at 4 KiB granularity. A 16 KiB host page uses the union of permissions of
its guest pages. FEX uses software TSO and Raven enables vector TSO by default. Raven forwards `FEX_*` environment
variables to the module except `FEX_SMALLTSCSCALE`, so settings can change translation and ordering behavior, including
the TSO defaults. The module's default timestamp-counter scaling matches Raven's `QueryPerformanceCounter` calculation.

A WoW64 program gets a 2 GiB address limit unless its image is large-address-aware. Raven raises the limit to 4 GiB for a
program that ships its own mtld3d. `RAVEN_LARGE_ADDRESS_AWARE=1` forces the raised limit, and `0` suppresses the
automatic raise and follows the image's flag. Image placement and later allocations use the same limit.

FEX calls supported graphics unix libraries as arm64 code. See [graphics setup](../GRAPHICS.md#fex-graphics),
[the optional code cache](code-cache/DESIGN.md) and [macOS limitations](../../../../docs/macos.md#known-limitations).

## Tests

```sh
ctest --test-dir build-macos -j1 --output-on-failure -L fex
```

This covers module loading, guest entry, NT calls, exceptions (`native_exception_*`), memory, thread state
(`native_thread_sync_contract_*`) and code invalidation. Configure with both modules and the x86 MinGW compiler to
register the WoW64 fixtures, labelled `wow64` ([wow64_tests.cmake](wow64_tests.cmake)). Without modules, the process
tests skip with status 77 and print the provisioning command, and the variants that need a module for one width are
not registered. Provision the modules before relying on the FEX suite.

## FEXCore changes

The patch series includes fixes to divide fault handling (`0008`), EFLAGS reconstruction (`0012`), call and return cache
invalidation (`0041`) and exit-link label binding (`0069`). These are FEXCore changes independent of the Raven host ABI.

An arm64 Linux backend would need Linux implementations of the memory mappings, JIT permissions and signal handling used
by the module host. The Darwin companion and its `x18` handling are specific to macOS.
