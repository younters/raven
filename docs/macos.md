# Raven on macOS

Raven runs x64 and 32-bit x86 Windows programs on Apple silicon. The arm64 host provides Windows system services
and an AppKit desktop. Rosetta 2 is the standard execution engine. [FEX](../native/platform/darwin/fex/README.md)
is an experimental engine selected per launch.

## Requirements

- An Apple silicon Mac. Development testing has used an Apple M2 on macOS 26.7.2. The minimum supported macOS version is unknown.
- Rosetta 2 for native execution. Install it with `softwareupdate --install-rosetta` before running `setup.sh`, which
  stops with that command when it cannot start an x86_64 program.
- Xcode command line tools, Homebrew, CMake 3.20 or newer, a C++20 compiler, Python 3.11 or newer, and Rust with `cargo`.
- MinGW-w64 13 or newer for both x86_64 and i686 to build the Windows components and tests.
- The LunarG Vulkan SDK, including the loader and MoltenVK, for Vulkan. Install it separately.
- Space for the build, the external components and the Skia source and build.

## Build

From the Raven source directory:

```sh
native/platform/darwin/setup.sh
```

The script installs missing Homebrew packages (`cmake`, `ninja`, `meson`, `python`, `mingw-w64`, `lld`, `llvm`,
`openssl@3`, `libpng`, `freetype`, `pixman`, `libxkbcommon`, `bison` and `zstd`). It builds Wine's `widl` tool,
creates a Python environment with Pillow, provisions RTL, common controls and installer services at the applicable widths,
and installs root certificates, libpng and graphics components. It builds CPU Skia and installs Liberation fonts for GDI.

The defaults are `build-macos` for the build and `../raven-components` and `../raven-skia` for external inputs, relative to the
source directory. Pass a build directory as the first argument. Set `RAVEN_COMPONENTS` and `RAVEN_SKIA` to choose the
component and Skia directories. On repeat runs the script reuses completed provisioning steps. For a build with
only selected components, configure directly. The CMake equivalent below assumes the setup outputs are already present.
Omit the arguments for components you do not need.

```sh
components="${RAVEN_COMPONENTS:-$PWD/../raven-components}"
skia="${RAVEN_SKIA:-$PWD/../raven-skia}"
font_hashes="$(shasum -a 256 "$components/fonts/LiberationSans-Regular.ttf" "$components/fonts/LiberationMono-Regular.ttf" "$components/fonts/LiberationSerif-Regular.ttf" | awk '{print $1}' | paste -sd ';' -)"
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPython3_EXECUTABLE="$components/python/bin/python3" \
  -DRAVEN_SKIA_SOURCE="$skia/src" -DRAVEN_SKIA_LIBRARY="$skia/out/libskia.a" \
  -DRAVEN_FONT_DIRECTORY="$components/fonts" -DFONT_HASHES="$font_hashes" \
  -DRAVEN_RTL_COMPONENT_PATH="$components/rtl/raven-rtl.dll" \
  -DRAVEN_CONTROLS_COMPONENT_DIR="$components/controls" -DRAVEN_CONTROLS32_COMPONENT_DIR="$components/controls32" \
  -DRAVEN_INSTALLER_COMPONENT_DIR="$components/installer" -DRAVEN_INSTALLER32_COMPONENT_DIR="$components/installer32" \
  -DRAVEN_ROOT_CERTIFICATE_DIRECTORY="$components/roots" -DRAVEN_DXMT_ROOT="$components/dxmt" \
  -DRAVEN_DXVK_PE64_DIR="$components/graphics/dxvk/pe64" -DRAVEN_DXVK_PE32_DIR="$components/graphics/dxvk/pe32" \
  -DRAVEN_VKD3D_PE64_DIR="$components/graphics/vkd3d-proton/pe64" \
  -DPNG_LIBRARY_RELEASE="$components/libpng/install/lib/libpng16.a" \
  -DPNG_PNG_INCLUDE_DIR="$components/libpng/install/include"
cmake --build build-macos
```

| Setting | Purpose |
|---|---|
| `RAVEN_FEX_MODHOST` | CMake option, off by default. `ON` builds the experimental FEX backend, its companion and tests. |
| `RAVEN_FEX_MODULE_ROOT` | CMake path to separately built FEX modules. The environment variable overrides it at launch. |
| `RAVEN_DXMT_ROOT` | CMake path to the DXMT PE and unix libraries. The environment variable overrides it at launch. |
| `RAVEN_DXMT_SOURCE_BUILD=1` | Environment setting for `setup.sh` to build DXMT from source. Set `RAVEN_COMPONENTS` and choose an external `RAVEN_DXMT_BUILD_ROOT` for its tools. |
| `RAVEN_KOSMICKRISP_ROOT` | CMake path to the pinned KosmicKrisp ICD for Vulkan tests. |
| `RAVEN_DARWIN_DXVK_TESTS` | Register strict DXVK comparisons, including unsupported graphics paths. Off by default. |
| `RAVEN_DARWIN_PRESENT_TEST` | Register Cocoa presentation tests that need a desktop session and Pillow. Off by default. |
| `RAVEN_DARWIN_HIDDEN_SURFACE_TEST` | Register the hidden Vulkan surface test for a desktop session. Off by default. |

For Direct3D 10 and 11, [DXMT requires a checked unix library](../native/platform/darwin/DXMT.md). The library installed
by plain setup is rejected by the Rosetta agent. See [graphics on macOS](../native/platform/darwin/GRAPHICS.md)
for component setup and test requirements.

## FEX modules

FEX is opt-in. Build the modules separately from Raven, then configure with `-DRAVEN_FEX_MODHOST=ON`:

```sh
modules="$PWD/../raven-components/fex-modules"
python3 native/platform/darwin/fex/scripts/build-fex-modules.py --raven-host --root "$modules"
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAVEN_FEX_MODHOST=ON -DRAVEN_FEX_MODULE_ROOT="$modules"
cmake --build build-macos
```

The [FEX component record](../native/upstream/fex/README.md) identifies the third-party Darwin source, exact revision,
patches and toolchain. The module builder installs the matched `xtajit64.dll` and `xtajit.dll` under the specified root.
See the [FEX backend documentation](../native/platform/darwin/fex/README.md) for module discovery and the host ABI.

## Run a program

```sh
export RAVEN_PREFIX="$HOME/.local/share/raven/my-app"
build-macos/native/raven --run-process --execution=native --desktop=macos --timeout-ms=600000 program.exe
```

In a build configured with FEX, use `--execution=fex` to select it. A build without FEX rejects that option. Windows threads run cooperatively by default. Set `RAVEN_PARALLEL=1` to let guest
threads run on separate pthreads. Windows child processes inherit the selected engine.

| Environment variable | Effect |
|---|---|
| `RAVEN_COMMAND_KEY=windows` | Map the Command key to the Windows key. The default maps it to Control. |
| `RAVEN_MACOS_FRAME=windows` | Draw guest window frames in place of the native Cocoa frames. |
| `RAVEN_XKB_LAYOUT` | Override the keyboard layout inferred from the macOS input source, for example `de` or `us(dvorak)`. |
| `RAVEN_LARGE_ADDRESS_AWARE=0` or `1` | Under FEX, `1` gives every 32-bit program the 4 GiB limit; `0` follows each image's large-address-aware flag. |
| `RAVEN_TRACE_COCOA=1` | Trace Cocoa window and input events. |
| `RAVEN_PROFILE_CONTROLLER=1` | Report time spent in Rosetta controller operations. |

### Finder

`native/platform/darwin/install-launcher.sh [build-directory] [--no-default]` installs `~/Applications/Raven.app`.
By default it also makes Raven the default app for `.exe` files. A Finder launch uses a prefix named after the program
under `~/.local/share/raven` and logs to `~/Library/Logs/Raven`.

Run `native/platform/darwin/raven-apps sync /path/to/prefix` to create app bundles for executable Start Menu and Desktop
shortcuts in the prefix. The launchers live under `~/Applications/Raven`. They refer to the executable and helper
at the time they are generated. Reinstall or sync them again after moving the build or source directory.

## Test

```sh
ctest --test-dir build-macos -j1 --output-on-failure
ctest --test-dir build-macos -j1 --output-on-failure -L fex
python3 native/platform/darwin/tests/apps_test.py
```

Run tests serially to avoid contention over shared fixtures and desktop resources. Some test families register separate
`_native` and `_fex` variants when FEX is configured. Without provisioned modules the FEX process tests skip with
status 77. The x87sidecar tests register only when the `RAVEN_X87_SIDECAR` cache path names a provisioned sidecar.
Tests that depend on WindowServer, default
audio devices or microphone authorization can skip with status 77. CTest disables `native_confine`, `native_warm_start`
and `native_vulkan_queued_extensions_x64` on macOS.

Avoid launching the Rosetta runtime or its tests under LLDB or debugserver. A debugged launch can stall the Rosetta
translation service (`oahd`). Use `sample <pid>` to inspect a running host, or send the host `SIGABRT` to get a
crash report under `~/Library/Logs/DiagnosticReports`. Sending `SIGABRT` ends that process.

## Runtime structure

`toolchain.cmake` places Darwin shim headers ahead of the SDK headers. The shims implement selected Linux interfaces
using Darwin facilities. `darwin.cmake` selects the Darwin implementations of `NativeProcess`, shared pixels,
confinement and the bootstrap. The NT layer and desktop use shared interfaces with Darwin-specific hooks.

| Area | Implementation |
|---|---|
| Rosetta process | `native_process.cpp` launches the x86_64 Mach-O `raven-bootstrap` under Rosetta. Signal handlers in `guest/agent.c` catch guest stops and exchange registers and requests with the arm64 controller through `guest/protocol.h`. PE32 code runs in compatibility mode in the same agent, with LDT selectors. |
| Memory | The controller maps host aliases for guest images and allocations. Host-page-aligned private ranges can use Mach VM remapping, and smaller views can use file-backed mappings. Executable writes go through the agent so Rosetta sees them. Raven tracks Windows reserve, commit and protection in the NT layer. |
| FEX process | The [FEX host](../native/platform/darwin/fex/README.md) runs the translator inside the arm64 companion and hands stopped Windows thread state back to Raven. |
| Windows and input | `cocoa.mm` supplies AppKit windows, input and waits through Raven's presentation callbacks. See [Dock policy](../native/platform/darwin/DOCK_POLICY.md), [cursors and DPI](../native/platform/darwin/CURSORS_HIDPI.md) and [live resize](../native/platform/darwin/RESIZE.md). |
| Graphics | [GDI presentation](../native/platform/darwin/GDI_PRESENTATION.md) uses IOSurface and Metal. The [Vulkan bridge](../native/platform/darwin/GRAPHICS.md#vulkan) presents through Metal surfaces. DXMT, mtld3d and Mullion provide separate Direct3D paths. |
| Audio and printing | [CoreAudio](../native/platform/darwin/audio/README.md) serves host-side audio APIs. [Printing](../native/platform/darwin/PRINTING.md) uses raster GDI pages, CoreGraphics PDFs and AppKit printing. |

## Known limitations

- **Isolation.** macOS has no OS-level Raven sandbox. The NT layer checks paths, but the host and guest lack a syscall
  filter. Under FEX, x64 guest code shares the Raven address space. The optional FEX code cache trusts its directory
  and loads executable code from it. Cross-process IOSurface forwarding authenticates peers by audit token,
  executable path, user and prefix; it assumes every controller comes from the same installation.
- **Platform dependencies.** Intel Macs and arm64 Linux are unsupported. Apple says general Rosetta availability
  extends through macOS 27 and narrows to certain older games on macOS 28. FEX depends on XNU continuing to
  preserve `x18` for the companion's legacy SDK declaration. [Apple's Rosetta documentation](https://support.apple.com/en-us/102527)
- **Linux interfaces.** The shims model the operations Raven uses. For example, file seals are unenforced, event
  descriptors use FIFOs, sequenced packet sockets use stream framing, and process data is limited by host inspection
  permissions. Network-interface and route change subscriptions return handles without delivering notifications.
  The port also uses shared memory, threading, loader and desktop code that runs on Linux.
- **Memory.** Windows 4 KiB pages sharing a 16 KiB FEX host page can inherit broader hardware permissions than Raven's
  bookkeeping records. FEX does not support guest guard pages. Pagefile-backed sections are charged per mapped view.
- **Execution.** The macOS warm host and hardware write watchpoints are unavailable. Rosetta's LDT selectors are not
  recycled, so a 32-bit process can exhaust them after about 8,000 thread creations. The Rosetta parallel channel has
  256 slots. Terminating a thread in a native call can strand locks it holds. The controller ends a Rosetta process
  whose agent leaves a service request unanswered for ten seconds, which heavy host swapping can trigger. It also
  suppresses up to 64 identical repeated breakpoint stops per resume, so a program that deliberately repeats a
  breakpoint with restored registers can miss stops. A fault inside a native graphics library during a unix call
  becomes a guest access violation, which can leave that library's locks held. Some host writes to code pages briefly
  remove execute permission, so another parallel guest thread on that page can fault. The desktop event queue ends the
  process if more than 10,000 events go unconsumed.
- **FEX coverage.** The modules are pinned to a separate build and FEX loads one translator instance per host process.
  Queued graphics and queued served calls are unavailable. x64 runtime hooks that copy the first 14 bytes of an export
  into a trampoline can stop the guest. A scheduler pause can stall if a parallel guest thread blocks inside a native
  call. Upstream CI has no FEX coverage.
- **Desktop.** DPI is chosen once per process, with no per-monitor transition, mixed-awareness virtualization or
  Per-Monitor v2 child notifications. Some cursors and theme sizes are approximated. Animated cursors and file-based
  cursor loading are unavailable. Direct launches use optional private macOS symbols for [application identity](../native/platform/darwin/DOCK_POLICY.md#application-identity).
- **Graphics.** Parallel GDI presentation copies pixels. Hosted Metal views can lag during live resize.
  DXMT and DirectComposition require separate components and are x64 only. DXMT tessellation is incomplete.
  The pinned DXVK and vkd3d-proton backends lack required features on the tested Mac Vulkan drivers.
  Raven has no macOS OpenGL backend.
- **Audio.** The audio APIs run in the host. The mixer implements basic XAudio2 with limited submix routing
  and no effects or compressed voices. Parallel callbacks wait for the selected thread to reach an API stop. A
  single-object wait with a timeout delivers pending XAudio2 callbacks; multiple-object, message and condition-variable
  waits can delay them until the wait ends.
  Voice-processing pass and voice-error callbacks are unavailable. Recording requires macOS microphone permission.
- **Printing.** PDF jobs contain raster pages. Printing supports `MM_TEXT` mapping. Dialog hooks, custom templates,
  AbortProc, PrintDlgEx callbacks and asynchronous spooler notifications are unavailable. A staging failure can surface
  during page output after `StartDoc` has returned a job ID.

## Third-party components

| Component | Source and terms |
|---|---|
| DXMT | Release v0.80 is MIT with additional dependency notices. The pinned source overlay is LGPL-2.1-or-later. [DXMT record](../native/upstream/dxmt/README.md) |
| FEX modules | MIT FEX-darwin with its dependency notices and Raven patches. [FEX record](../native/upstream/fex/README.md) |
| mtld3d | zlib, patched for Raven. [mtld3d record](../native/upstream/mtld3d/README.md) |
| Mullion | LGPL-2.1-or-later, patched for Raven. [Mullion record](../native/upstream/mullion/README.md) |
| x87sidecar | MIT. [x87sidecar record](../native/upstream/x87sidecar/README.md) |
| MoltenVK and KosmicKrisp | MoltenVK is Apache-2.0. KosmicKrisp uses MIT source files. Both come from the LunarG SDK. [KosmicKrisp record](../native/upstream/kosmickrisp/README.md) |
| Skia and Liberation fonts | BSD-style Skia terms and compiled dependency notices. Liberation uses SIL OFL 1.1. [Component notices](../LICENSES.md) |
| Wine build tools | `setup.sh` builds `widl` from Wine 10.0. See the source archive's notices and LGPL terms. |

See the [FEXCore patch notes and arm64 Linux requirements](../native/platform/darwin/fex/README.md#fexcore-changes)
for translation work shared with other possible hosts.
