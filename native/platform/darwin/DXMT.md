# Direct3D through a guest-side Metal driver

The x86_64 guest can call Darwin libraries on its main thread. `guest/unix_call.s` implements the Win64 unix-call entry
`NTSTATUS (uint64_t handle, uint32_t code, void *args)`. The handle is a registered array of Darwin SysV functions, each taking
one argument pointer. The agent service loads an explicitly supplied host library and finds `__wine_unix_call_funcs`.
The caller must supply the table length from the pinned library's ABI; neither the guest nor a DLL search path chooses a native library.
Libraries stay loaded until the agent exits. WoW64 tables and native-to-PE callbacks are not implemented.

The gate preserves Win64's nonvolatile GPRs, XMM6-15 and floating-point control state. It uses a separate 2 MiB guarded stack,
restores the agent's TSD before calling Darwin, and restores the Windows GS base before returning. Signal handling keeps the guest's
GS base separately: Rosetta's signal context reports zero for native GS, so the two assembly transitions identify the actual base.
The scheduler defers switching Windows threads while the gate or its native function is active. Deadlines still terminate the agent.

`darwin_unix_call` runs an import-free PE with a real TEB, a tiny x86_64 Mach-O unixlib, and 1 ms scheduler interrupts. It verifies
native pthread/TLS access, stack separation, GS restoration, all Win64 nonvolatile integer/vector registers, repeated calls,
NTSTATUS results and rejection of unknown handles and function indices. It also checks that a unixlib allocation is readable by host
APIs and that repeated native interrupts do not restart a call's deadline.

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
8-bit RGBA/BGRA SDR surfaces. HDR, minimum-duration/vsync pacing and WoW64 are not implemented by this phase.

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
