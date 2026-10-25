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
NTSTATUS results and rejection of unknown handles and function indices. This is phase (a); it does not imply Direct3D works.

## Selected driver and presentation design

DXMT v0.80 is pinned at `589adb780354b461645b29999cefaf533594ee99`. Its PE `winemetal.dll` asks
`NtQueryVirtualMemory(..., MemoryWineUnixFuncs = 1000, ...)` for its table and resolves the **data** export
`__wine_unix_call_dispatcher` from ntdll. That export contains the address of the gate. The unixlib's single argument functions
operate on the same argument structures as the PE thunks, so pointers do not need marshalling between address spaces.

The release unixlib links to Wine's `winemac.so` and `ntdll.so`; it cannot be used unchanged in Raven. The Raven adapter must replace
the window-driver boundary and implement native completion notifications (`NtSetEvent`) without loading Wine's ntdll.

Presentation will use IOSurface-backed Metal textures in the guest. After command completion, an IOSurface Mach send right and the
HWND go to the host, which imports it and sets the corresponding window layer's contents. Acknowledgement and a bounded buffer pool
must prevent reuse before the compositor has consumed a frame; resizing and closing must retire in-flight surfaces. This uses public
APIs, works across architectures, and avoids private `CAContext`/`CALayerHost` dependencies. Host-owned NSView/CAMetalLayer pointers
are not valid in the guest. This presentation path is not implemented by the gate spike.

## Reproduce

Configure and build the normal macOS tree, then:

```sh
ctest --test-dir build -j1 --timeout 120 -R '^darwin_unix_call$' --output-on-failure
```

No debugger is needed or supported for these tests. Never launch the runtime or agent under lldb/debugserver on this host.
