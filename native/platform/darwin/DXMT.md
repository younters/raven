# Direct3D through a guest-side Metal driver

The x86_64 guest can call Darwin libraries on each Windows pthread. `guest/unix_call.s` implements the Win64 unix-call entry
`NTSTATUS (uint64_t handle, uint32_t code, void *args)`. The handle is a registered array of Darwin SysV functions, each taking
one argument pointer. The agent service loads an explicitly supplied host library and finds `__wine_unix_call_funcs`.
The caller must supply the table length from the pinned library's ABI; neither the guest nor a DLL search path chooses a native library.
Libraries stay loaded until the agent exits. WoW64 tables and native-to-PE callbacks are not implemented.

The gate preserves Win64's nonvolatile GPRs, XMM6-15 and floating-point control state. Each pthread owns a separate 2 MiB guarded stack. The gate
restores that pthread's Darwin TSD before calling Darwin, and restores the Windows GS base before returning. Signal handling keeps the guest's
GS base separately: Rosetta's signal context reports zero for native GS, so the two assembly transitions identify the actual base.
Before restoring TSD, the gate uses the TSD-independent `thread_selfid` syscall to find its pthread record. Records publish the
native stack, TSD, Windows GS location and channel's active flag before entering PE code; joining a pthread retires its identity.
The existing alternate-stack signal entry selects only Windows channels. Native Metal/dispatch workers have no such stack and
ignore scheduler signals. The default scheduler defers logical switches while that channel's gate or native function is active;
parallel channels continue independently. Deadlines still terminate the agent.

`darwin_unix_call` runs an import-free PE with a real TEB, a tiny x86_64 Mach-O unixlib, and 1 ms scheduler interrupts. It verifies
native pthread/TLS access, stack separation, GS restoration, all Win64 nonvolatile integer/vector registers, repeated calls,
NTSTATUS results and rejection of unknown handles and function indices. It also checks that a unixlib allocation is readable by host
APIs and that repeated native interrupts do not restart a call's deadline. `darwin_unix_call_parallel` additionally rendezvous two
Windows pthreads inside native calls, checks distinct stacks/TSD and preserved state on both, and sends SIGUSR1 to native workers.
After a unixlib is loaded, the host requests 1 ms wakeups to pump frames and completion events while parallel guest channels run,
as well as pumping during Windows waits. Programs without a unixlib retain the ordinary scheduler wait budget.
IOSurface child layers inherit the existing Cocoa graphics layer's contentsScale; Cocoa retains control of DPI and resize pacing.

## Selected driver and presentation design

DXMT v0.80 is pinned at `589adb780354b461645b29999cefaf533594ee99`. Its PE `winemetal.dll` asks
`NtQueryVirtualMemory(..., MemoryWineUnixFuncs = 1000, ...)` for its table and resolves the **data** export
`__wine_unix_call_dispatcher` from ntdll. That export contains the address of the gate. The unixlib's single argument functions
operate on the same argument structures as the PE thunks, so pointers do not need marshalling between address spaces.

The release unixlib links to Wine's `winemac.so` and `ntdll.so`; the provisioner replaces those dependencies with Raven's adapter.
The adapter sends `NtSetEvent` completion notifications through a bounded shared queue, drained by the host scheduler. It replaces
v0.80's listener table entries 104 and 108-110 with a dispatch-queue listener: upstream's `SharedEventListener_start` enters a permanent
`CFRunLoopRun`, which would occupy Raven's only real guest thread and starve every other Windows thread. The Windows listener thread
can return because the native dispatch queue owns delivery. No callback enters PE code on a native worker thread.

Once a unixlib is loaded, host memory access also validates unixlib/Metal allocations against the guest Mach task's VM permissions.
Known Windows mappings retain their existing protection checks. This is needed when a PE program passes a Metal staging pointer to
`WriteFile`; the Windows allocator did not create that pointer. Loaded agent/unixlib code is covered by the same VM validation.

Phase (b) works: `darwin_dxmt_device` creates a hardware D3D11 device, immediate context and vertex buffer, and `darwin_dxmt_triangle`
runs the existing `testdata/d3d11/triangle.c` and passes the pixel reference comparison. Neither test copies graphics DLLs beside its
program: Raven resolves the four driver DLLs through the configured DXMT root. Program-local DLLs still take precedence.

Phase (c) works through public IOSurface APIs. The adapter replaces the pinned driver's drawable/window table entries with a
three-buffer IOSurface-backed Metal texture pool per swap chain. GPU completion publishes a Mach send right, HWND and sequence in
the control block. The host uses `mach_port_extract_right` on the guest task, imports the surface, and sets an ordinary child
CALayer's contents inside the existing Cocoa graphics surface. No NSView/Metal object pointer crosses processes, and no private
`CAContext`/`CALayerHost` API is used.

The host holds an IOSurface use count while a frame is displayed. Replacing the contents acknowledges the previous buffer; the
guest also requires `IOSurfaceIsInUse` to be false before reuse. A live IOSurface Mach port itself increments global use count, so
ports are created after GPU completion and dropped by the guest after import acknowledgement. A separate importing state prevents
the guest from dropping the port while the host is copying it. GPU completion blocks retain their drawable; retirement acknowledges
port destruction, and resizing allocates a fresh pool. The shared table has 16 slots across all swap chains.

The host polls presentation both during guest execution and while Windows threads wait. It uses the existing served-call memory
access discipline while the native guest is waiting, avoiding a buffer-pool/host-pump deadlock. `RAVEN_TRACE_DXMT=1` reports frame
counts; `RAVEN_DXMT_CAPTURE_DIRECTORY` optionally records imported RGBA frames for diagnosis.

`darwin_dxmt_window` runs the existing `testdata/d3d11/window.c` under `--desktop=macos`. All 310 frames presented with zero errors.
The test compares imported surface pixels **and actual per-window screen captures** with GPU readback at 256x256 and after resizing
to 320x200. Evidence is in `build/native/dxmt-window/{last,resized}-{window,client}.png`. The public transport currently supports
8-bit RGBA/BGRA SDR surfaces. Minimum-duration pacing is implemented in phase (d); hardware-vblank synchronization, HDR and WoW64
remain outside this transport.

## Runtime services and remaining blocker

Phase (d) adds Raven-built `d3d10.dll` and `d3d10_1.dll` creation entry points, forwarding hardware-device creation to DXMT's
`D3D10CoreCreateDevice` and creating swap chains through the device's DXGI factory. `darwin_dxmt_device10` verifies both versions,
feature-level selection, clear/copy/map readback, invalid SDK rejection and real-window swap-chain creation/presentation/teardown.
These small DLLs provide the creation APIs; D3D10 shader/effect utility exports are not implemented by them.

Wine's optional `__wine_dbg_output` now writes driver diagnostics to Raven's stderr. `strncpy` is exported before a late-loaded
`winemetal.dll` needs it (initial imports alone did not publish it). Metal validation/capture environment variables explicitly set
on the host are forwarded to the native agent. Native faults report a module and backtrace without a debugger.

Autorelease scopes need special handling because logical Windows threads share one pthread in the default scheduler: their scopes
may overlap without nesting. The adapter returns independent scope tokens and drains a native pool only when every overlapping
scope on that pthread ends. Both the pool and scope count use Darwin thread-local storage for parallel mode.
`darwin_dxmt_pool` deliberately releases the older of two scopes first and verifies that the second scope's NSString survives.
MSAA and MRT fixtures also passed three cold-cache runs each after this change.

Minimum-duration presents retain their completed drawable until a native dispatch deadline, leaving the host pump and GPU event
delivery available. `darwin_dxmt_pacing` uses interval-four presents and an ordered GPU event to verify execution, and compares QPC
against GetTickCount64. This exposed an existing clock shim bug: `"isb; mrs ..."` assembled only `isb`, leaving an arbitrary register
as the timestamp. A newline fixes it. Independent arm64 and x86_64 probes both measure the raw counter at approximately 1 GHz;
the Mach timebase scaling for `mach_absolute_time` must not be applied to this counter.

Final serial verification: 22 of 24 selected tests passed. The 16 Darwin tests comprise 15 passes and the tessellation failure
below. CPU, memory mapping, x86, provider, environment and x64/x86 served-semaphore regression tests passed. The other failure,
`native_memory_contract`, is an unchanged baseline expectation: its test rejects MEM_TOP_DOWN (`0x100000`), while the existing
allocator explicitly strips that bit and accepts the allocation (`native/nt/memory.cpp:155`). The earlier shared-mapping test also
fails while enumerating the absent `/proc/self/fd`. Neither existing test nor allocator was changed for this work.

The final pacing probe measured 16 interval-four presents through GPU completion in about 1.03 s; the 2,000-frame rate fixture
measured about 149 presents/s on this run. These are local observations, not performance guarantees.

The original deferred, geometry, textured, compute, MSAA, MRT/mipmap/indirect/query, fullscreen and presentation-rate fixtures are
registered as hardware tests. The original tessellation fixture is a known failing test, kept strict and enabled. It returns OK
but reads back only the clear color (zero triangle pixels). Disabling culling in an uncommitted diagnostic variant did not change
that result. With Metal GPU validation enabled, the original fixture reports:

```text
Frame 0: Invalid load in 'threadgroup' address space at offset 720, executing object function:
"vshs_c2b02776_3d2039ae43d161244eabdd7867aa3c9e4507280e"
Error detected in drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:
```

This is the blocker in the pinned driver's generated tessellation GPU path. Its root cause has not been isolated to shader
generation versus dispatch inputs; no speculative shader patch or relaxed pixel expectation is included. Reproduce with:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build -j1 --timeout 120 \
  -R '^darwin_dxmt_tessellation$' --output-on-failure
```

Validation evidence from this run is `build/native/dxmt-tessellation-validation.log`. The shared IOSurface table has a fixed
capacity, and shutdown of the agent releases remaining native resources.

## Shared D3D11 textures and browser presentation

Raven implements DXMT's D3DKMT adapter, device, allocation, legacy shared resource and NT shared handle calls in
`dxmt/kernel_thunks.mm`. A shared allocation records DXMT's private runtime data in the prefix; DXMT publishes its IOSurface
Mach port under the name in that data. NT shared handles are ordinary Raven file handles, so handle inheritance and
`DuplicateHandle` use the existing process channel. `darwin_dxmt_shared` creates both kinds of texture, opens them on a
second D3D11 device and in another Raven process, and checks GPU readback. The fixture covers 64-bit guests; DXMT's
WoW64 unix-call table is not available yet.

DXMT v0.80 rejects a swapchain whose HWND belongs to another process. The pinned provisioner applies the source-equivalent
patch in `dxmt/patches/0001-allow-foreign-hwnd-swapchain.patch` to the release's `d3d11.dll`, verifying the complete input
and output hashes. Raven forwards the resulting IOSurface Mach port to the HWND owner's presentation service. Chrome's
sandboxed GPU process can then present D3D11 frames in its browser process. Provision a new external DXMT overlay with
`scripts/provision-dxmt.py` to get this patch; an older overlay retains the rejection.

### Opt-in DXGI identity

`RAVEN_DXGI_VENDOR=amd` reports AMD `1002:7340` and “AMD Radeon Pro 5300M” through both
`EnumDisplayDevices` and DXMT's `IDXGIAdapter::GetDesc`. `nvidia` reports `10de:1e84`, `intel` reports
`8086:3e92`, and a four-digit hex vendor (with optional `0x`) reports that vendor and device `0001`.
The host passes DXMT's existing `dxgi.customVendorId`, `dxgi.customDeviceId`, and `dxgi.customDeviceDesc`
options through the guest Windows `DXMT_CONFIG` environment variable; existing options from
`RAVEN_GUEST_DXMT_CONFIG` or `DXMT_CONFIG` are retained. The
display's SetupAPI driver key resolves to a process-local registry view with driver version
`31.0.22000.11000` and date `10-01-2025`; this does not write the prefix hive. Without the setting,
the display identification remains unchanged. `raven-apps` selects `amd` by default for `firefox.exe`
and permits an explicit caller override. These identifiers are compatibility declarations for DXMT,
not the Mac's physical GPU model.

The pinned DXMT version creates textures with `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` but has a TODO for the mutex and
does not implement `IDXGIKeyedMutex::AcquireSync` or `ReleaseSync`. Its named NT share and shared fence paths are also
not covered by the fixture. Firefox's DXMT WebGL path additionally needs `d3dcompiler_47.dll` in the private browser
prefix; this is an application dependency, not a DXMT provisioner file.

## Reproduce

Configure and build the normal macOS tree, then:

```sh
ctest --test-dir build -j1 --timeout 120 -R '^darwin_unix_call$' --output-on-failure
```

To enable the pinned driver and hardware tests:

```sh
python3 scripts/provision-dxmt.py --destination /absolute/path/outside/checkout/graphics
cmake -S . -B build -DRAVEN_DXMT_ROOT=/absolute/path/outside/checkout/graphics/dxmt
cmake --build build
ctest --test-dir build -j1 --timeout 120 -R '^darwin_(unix_call|dxmt_)' --output-on-failure
```

The `RAVEN_DXMT_ROOT` environment variable overrides the build-time root. WoW64 remains unsupported. Native VM checks become available
only after a library is loaded. The shared-mapping regression test currently fails before testing mappings because it enumerates
Linux's `/proc/self/fd`; CPU, memory and x86 contracts pass (one CPU cleanup run failed transiently and passed on rerun).

No debugger is needed or supported for these tests. Never launch the runtime or agent under lldb/debugserver on this host.


## Parallel integration verification (2026-10-25)

The saved four-part DXMT series and interrupted parallel work were restored, reviewed and rebased onto `macos` at `c2caa26`.
The WIP commit was replaced with ordinary implementation/test commits. Build and test commands used `-j8` and `ctest -j1
--timeout 120` respectively. The adapter was rebuilt only in `/tmp/raven-dxmt-parallel-overlay/dxmt`; shared component directories
were not modified. Configure with `-DRAVEN_DXMT_ROOT=/tmp/raven-dxmt-parallel-overlay/dxmt` to use that overlay. The existing
DXVK/vkd3d CMake roots were enabled in both comparisons so their unsupported fixtures remained visible.

The untouched macOS build's full baseline registered 290 tests: 257 passed, 29 failed, three skipped and one disabled. This build
predated the concurrent `_EH_prolog` and context-menu commits. The first complete integrated run, on the `_EH_prolog` base, registered
307 tests: 273 passed, 30 failed, three skipped and one disabled. No previously passing baseline test regressed in that run.
The final complete run after rebasing onto `c2caa26` had 272 passes, 31 failures, three skips and one disabled test. Its extra failure
was `native_dynamic_loader_contract` (generic failure-contract mismatch); it passed in the baseline and first integrated full run.
That test gives each runtime case a 500 ms deadline. The raw failed run is retained rather than replacing its count with rerun results.

With `RAVEN_PARALLEL=1`, all 23 selected Darwin/CPU/DXMT tests completed: 22 passed and only the already-known DXMT tessellation
pixel comparison failed. All 14 DXMT workloads that passed on the old branch passed in both modes: D3D10/10.1 device/readback,
D3D11 device/readback, pool scopes, pacing, triangle, window, deferred, geometry, texturing, MSAA, compute, MRT/mipmap/indirect/query,
fullscreen and present-rate. Both gate registrations and CPU, Darwin parallel, compatibility, runtime, system API and cursor/DPI
contracts passed. The expanded pool fixture holds native scopes on two Windows threads and releases the older scope while the
other thread still owns an autoreleased string.

The window fixture matched imported IOSurface pixels and actual Cocoa screen captures against GPU readback at 256x256 and 320x200.
Each mode presented all 310 frames with zero presentation errors. Logs, JUnit results and separate screenshots are saved under
`build/dxmt-validation/{default,final-default,parallel}/`. The final recheck log is saved alongside those directories.

The 29 baseline failures comprise 23 DXVK/vkd3d hardware fixtures and six existing contracts: memory, layout-failure component,
coordinates, desktop icon, string resources and DLL lifecycle. DXMT tessellation remains strict and enabled with its documented
clear-only output. Hardware vblank synchronization, HDR and WoW64 retain the limitations above. Cross-process D3D resource
sharing was added after that 2026-10-25 comparison, as described above.

Existing files outside the Darwin platform directory have these final line deltas against `macos`:

- `native/cpu/native_cpu.h`: +12 (Darwin-only declarations).
- `native/main.cpp`: +8 (Darwin module lookup).
- `native/nt/providers.cpp`: +31 (Darwin dispatch, events and presentation integration).
- `native/graphics/backends.json`: +11 (pinned driver manifest).
- `scripts/provision-graphics.py`: +7/-1 (Darwin provisioner integration).

The new `scripts/provision-dxmt.py` has 78 lines. New DXMT provenance and license files total 861 lines. There is no final diff in
`cocoa.mm`: IOSurface layer scaling lives entirely in `dxmt/presentation.mm` and inherits the existing graphics layer's contentsScale.

### Experimental source overlay (Firefox investigation)

The opt-in source provisioner pins `3b252590cb0290e36fca6f088b5255b871d2d95a`, which contains
keyed-mutex merge `e520fea415ba4b82b0c346dae77bbb1be4897453` (upstream PR #195).
It applies the foreign-HWND source patch and links the unixlib directly to Raven's adapter instead
of Wine's unix libraries. Two additional source patches correct the non-indexed geometry argument
sentinel and preserve signed 100 ns keyed-mutex timeouts (including infinite waits). Patch 0005 makes
AcquireSync/ReleaseSync own the shared event their encoder command captures; upstream retains it only when
the encoder runs, after Firefox's GPU-process checks have released the texture (use-after-free in
`objc_retain`, covered by `darwin_dxmt_self_share`). The normal binary-patched v0.80 path remains the default until runtime
qualification passes. Building the upstream test executables is not equivalent to passing them.

Prerequisites: Meson, the Windows cross compiler, Metal's Xcode toolchain, **x86_64** LLVM 15 static
libraries, and a private configured Wine build. Current DXMT still requires major version 15;
the Apple Silicon Homebrew support does not make arm64 LLVM archives suitable for this unixlib.
The existing local LLVM installation is `/Users/yount/Documents/Projects/raven-components-build/llvm15`.
Do not use an arm64-only Homebrew archive in its place.

For Wine 10 source already provisioned under `$C/tools/wine-src`, configure a fresh external directory:

```sh
export PATH="$C/tools/bin:$(brew --prefix bison)/bin:$PATH"
mkdir -p /tmp/raven-firefox-wine-build
cd /tmp/raven-firefox-wine-build
CC='clang -arch x86_64' CXX='clang++ -arch x86_64' "$C/tools/wine-src/configure" \
  --host=x86_64-apple-darwin --enable-win64 --disable-tests --without-x --without-freetype \
  --without-gstreamer --without-pcap --without-sdl --without-usb --without-vulkan --without-cups --without-gnutls
make -j4 BISON="$(brew --prefix bison)/bin/bison" tools/winebuild/winebuild \
  dlls/ntdll/x86_64-windows/libntdll.a dlls/dbghelp/x86_64-windows/libdbghelp.a \
  dlls/winecrt0/x86_64-windows/libwinecrt0.a
```

From Raven's checkout:

```sh
python3 scripts/provision-dxmt.py --destination /tmp/raven-firefox-source-components \
  --source-llvm ~/Documents/Projects/raven-components-build/llvm15 \
  --source-wine /tmp/raven-firefox-wine-build
```

The source build and submodules stay inside the chosen destination's `dxmt-source-build` directory;
that directory is provisioner-owned and reset to the pin on each invocation. Builds use `-j4`.
`dxmt/installed.json` records the source revision, patch hashes and installed file hashes. The source
overlay carries the pin’s LGPL license files and retained source separately from the MIT fallback’s notices.
The source table has 146 entries; provisioning verifies that its first 132 entries match the fallback
wrapper layout before publishing `unix64/unix-call-count`.
For setup.sh, set `RAVEN_DXMT_SOURCE_BUILD=1` and an explicit **private** `RAVEN_COMPONENTS`
destination. `RAVEN_DXMT_LLVM15` and `RAVEN_DXMT_WINE_BUILD` can reuse prepared toolchains.
Otherwise the source helper checks the existing x86_64 LLVM 15 installation, then a missing-only
Homebrew llvm@15 installation, and finally builds pinned LLVM 15.0.7 static libraries under
`~/Documents/Projects/raven-components-build/llvm15`. An arm64-only Homebrew installation is
not used for the x86_64 unixlib. Wine's build tools and import libraries stay in a private build
under that same build root; `RAVEN_DXMT_BUILD_ROOT` overrides it. All builds use `-j4` and nothing
is installed system-wide. The existing LLVM/Wine reuse path was tested; the LLVM-from-scratch
branch was not needed on this machine.


The new D3DKMT keyed-mutex implementation shares opaque runtime metadata, 64-bit keys and GPU
fence values across processes in the prefix's shared-object namespace. Guest waits yield to the
scheduler. Tests cover both pointer widths, same-process thread contention, cross-process handoff,
timeout and persistent abandonment. `darwin_dxmt_shared_keyed` additionally imports a real DXMT
texture into another process and checks synchronized GPU pixel readback; it explicitly skips on
the v0.80 fallback. NT-security keyed-mutex handles are not implemented and return
`STATUS_NOT_SUPPORTED`. Abruptly terminated creators can leave small metadata files in the test
prefix; normal destruction removes them.

#### System32 HLSL compiler

Windows ships `d3dcompiler_47.dll` in System32 and ANGLE loads it from there; Raven's built-in export only
refuses `D3DCompile`. The source overlay also builds `pe64/d3dcompiler_47.dll`
(`dxmt/build_d3dcompiler.py`): Wine 10's d3dcompiler objects linked against vkd3d-shader pinned at git
`dcefd8b0` (Wine's bundled vkd3d 1.14 crashes in copy propagation on WebRender shaders, and 1.19 rejects
ANGLE's statically indexed sampler arrays), with `d3dcompiler_crt.c` forwarding the stdio calls mingw headers
emit to ucrtbase. When the configured DXMT root carries this file it replaces the stub as a system module;
a copy beside the application still wins. `darwin_d3d_compile` checks vs_5_0/ps_5_0 DXBC and error blobs.
The compiler builds every WebRender shader, but some generated code is wrong: page content draws with
wrong colours and missing gradients/shadows, while Microsoft's compiler renders the same page correctly
on the same DXMT build. Hardware WebRender is therefore not yet correct with this compiler.

`raven-apps` selects the AMD compatibility identity for `firefox.exe`, including generated app
bundles launched from Launchpad, while preserving explicit caller overrides. Direct runtime launches
remain opt-in through `RAVEN_DXGI_VENDOR=amd`. This setting does not disable Firefox's sandboxes.
