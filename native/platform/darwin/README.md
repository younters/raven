# Raven on macOS (Apple silicon)

The host runtime is a native arm64 program. Windows code runs in `raven-bootstrap`, an x86_64 Mach-O that Rosetta 2 translates.
This tree is a fork that keeps tracking upstream, so the port lives here and changes upstream files as little as possible.

## Building

```sh
brew install cmake ninja python mingw-w64 lld openssl@3 libpng freetype pixman
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-macos
ctest --test-dir build-macos -j8
```

The Vulkan loader and `vk.xml` come from the LunarG SDK, which installs them under `/usr/local`.

## Layout

| Path | Role |
|---|---|
| `toolchain.cmake` | Included by the top-level CMakeLists before `native/`: shim include directory, `raven_darwin` library, OpenSSL 3, Windows-target clang |
| `darwin.cmake` | Included at the end of `native/CMakeLists.txt`: swaps the guest controller and the bootstrap for the macOS ones |
| `include/` | Linux headers the runtime includes, on top of the macOS facilities they map to; ahead of the SDK on the include path |
| `compat/linux.cpp` | The functions behind those headers (`raven_darwin`, linked into every host target) |
| `compat/sockets.cpp` | `SOCK_SEQPACKET` and abstract names for Unix sockets; defines `send`, `recv`, `sendmsg`, `recvmsg`, `bind`, `connect`, `close` |
| `compat/procfs.cpp` | The `/proc` files read as a process API, from libproc, sysctl and Mach |
| `compat/compat_test.cpp` | `darwin_compat_contract`: the interprocess behaviour of the two above |
| `native_process.cpp` | `NativeProcess` for macOS, built instead of `cpu/native_cpu.cpp` |
| `guest/` | The x86_64 guest agent and the zero-fill segment that keeps the low 4 GiB free for Windows images |
| `bin/raven-clang` | clang for Windows targets, with Homebrew's MinGW-w64 sysroot |

Upstream files changed for macOS: `CMakeLists.txt` and `native/CMakeLists.txt` (the platform gates and the two includes),
`native/main.cpp` (a `to_string` cast for libc++'s 128-bit file times) and `native/desktop/foreign_surface.cpp` (`__atomic_load_n`
where libc++ has no `atomic_ref<const T>`). Where the runtime reads a `/proc` file with an `ifstream` or lists `/proc`, an `#ifdef __APPLE__`
reads `raven_darwin::proc_stream` or `proc_directory` instead: `nt/child_process.cpp`, `nt/service_api.cpp`, `nt/process_info_api.cpp`
and `nt/winsock_api.cpp`, which also writes guest socket addresses in the macOS layout (length byte, then family byte). `nt/pipe.cpp`
asks `raven_darwin_unix_name_bound` where Linux reads `/proc/net/unix`, and `nt/file_api.cpp` reopens a file with `raven_darwin_reopen`.

## Status

- Phase 1, done: everything builds, and the tests that need no guest process pass.
- Phase 2, next: the guest controller. The agent catches its own traps and faults with signal handlers and trades register contexts
  with the host through shared memory; the host maps and reads guest memory through the agent's task port.

## Rosetta 2 facts the design relies on

Measured on macOS 27.2, M4 Pro.

- **Pages:** translated processes have 4 KiB pages, and protection is enforced per 4 KiB page.
- **Traps:** `int3` arrives as SIGTRAP, `ud2` as SIGILL, and a Windows `syscall` as SIGSYS. Faults arrive as SIGBUS or SIGSEGV with the
  fault address. Edited contexts resume correctly.
- **32-bit code:**
  - `i386_set_ldt` works only with an explicit index, one entry per call.
  - Far `call`, `retf`, `iretq`, and setting `cs` in a signal context all enter 32-bit mode. A plain far `jmp` does not.
  - FS with an LDT data selector addresses the TEB. Rosetta rejects nonzero GS selectors.
- **Thread environment block (TEB):** for 64-bit code, the GS base can be set with machdep call `0x3000003`.
- **AVX:** AVX, AVX2, FMA and BMI2 are reported only with `ROSETTA_ADVERTISE_AVX=1`. Signal contexts then carry the AVX state. There is no
  AVX-512.
- **Timestamp counter:** `rdtsc` in the guest reads the ARM generic timer `cntvct_el0` (1 GHz on M4). The host reads the same counter.
- **Across tasks:**
  - From an arm64 task, `mach_vm_read`, `mach_vm_allocate` and `mach_vm_protect` work on the guest.
  - `mach_vm_write` into the guest's executable pages and x86 `thread_get_state` do not.
- **Trap cost:** one trap round trip (signal, host answer, resume) costs about 23 µs. Calls Raven serves through shared memory avoid it.

## Linux behaviour still missing at run time

These compile, but do not yet behave as on Linux.

### Sockets and handles
- `SOCK_SEQPACKET` Unix sockets (message-mode named pipes, handle channels) are stream sockets that frame each send as a 32-bit length
  and the message, with descriptors on the frame's first byte. A send-buffer low-water mark of 256 KiB + 0x5ec marks such a socket in
  every process that holds it, and makes the kernel take each frame whole or not at all. Messages are limited to that mark less 4
  bytes. `read` and `write` on one see the frames; the runtime uses `recv` and `send`.
- Abstract Unix socket names are socket files in `$TMPDIR/raven-<uid>/`, one per socket type, beside a lock file whose
  open-file-description lock the binding process holds. A bind replaces the files of a process that ended without closing them.
- `/proc/net/tcp*` lists only the sockets of processes this user may inspect.
- `MSG_CMSG_CLOEXEC` is a flag `recvmsg` takes out, then marks the descriptors received close-on-exec.

### Memory, events and waits
- `SharedPixels` accepts Windows' 4 KiB section offsets through a host-page-aligned macOS mapping.
- `eventfd` is a FIFO: it wakes like an eventfd, but reading does not return the counter.
- File seals are accepted and not enforced.
- `ppoll` waits in milliseconds.

### Graphics, audio and system integration
- **Desktop / Vulkan:** `--desktop=macos` uses Cocoa windows and Metal child views. The Vulkan bridge presents through MoltenVK;
  see [VULKAN.md](VULKAN.md) for verified tests, the desktop integration test, and Direct3D backend limitations.
- **OpenGL:** the bridge uses EGL.
- **Audio:** uses PulseAudio.
- **Shell open:** uses `xdg-open`.
- **System fonts:** looked up under `/usr/share/fonts`.
