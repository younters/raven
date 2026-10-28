# Raven on macOS (Apple silicon)

The host runtime is a native arm64 program. Windows code runs in `raven-bootstrap`, an x86_64 Mach-O that Rosetta 2 translates.
This tree is a fork that keeps tracking upstream, so the port lives here and changes upstream files as little as possible.

The Cocoa cursor and Retina coordinate contract, current limitations, and tests are documented in [CURSORS_HIDPI.md](CURSORS_HIDPI.md).
Printer DCs, PDF jobs, native panels, test isolation and current limitations are documented in [PRINTING.md](PRINTING.md).
The graphics backend decision, KosmicKrisp qualification, and measured DXMT/DXVK comparison are in [GRAPHICS.md](GRAPHICS.md).
GDI window storage, Metal presentation, buffering and comparison controls are documented in [GDI_PRESENTATION.md](GDI_PRESENTATION.md).

## Building

```sh
native/platform/darwin/setup.sh          # Homebrew packages, every guest component, Skia and fonts, then configure and build build-macos
ctest --test-dir build-macos -j1         # tests share prefixes: one at a time
RAVEN_PREFIX=~/.local/share/raven/app build-macos/native/raven --run-process --execution=native --timeout-ms=86400000 \
	--desktop=macos program.exe
```

`install-launcher.sh` then installs `~/Applications/Raven.app` and makes it the default application for `.exe` files: Finder opens a
program in its own prefix, `~/.local/share/raven/<program name>`, from the program's folder, with its output in
`~/Library/Logs/Raven/<program name>.log`. `--no-default` installs the launcher without taking over `.exe` files.

Live resizing waits for the program to draw each size about as long as it usually takes (10-100 ms), sends the last size of a drag as
soon as the edge stops, and crossfades frames of a new size. `RAVEN_COCOA_RESIZE_MS=<n>` fixes the wait at n ms and
`RAVEN_COCOA_RESIZE_BLEND=<seconds>` sets the crossfade (`0` turns it off).

`setup.sh` puts what it downloads and builds beside the checkout (`../raven-components`, `../raven-skia`) and skips what is already
there. The Vulkan loader, `vk.xml` and MoltenVK come from the LunarG SDK, which installs them under `/usr/local`.

Never run `raven`, `raven-bootstrap` or a test under lldb/debugserver: debugging a process that starts a Rosetta-translated child can
wedge `oahd`, after which no x86_64 program starts until `sudo launchctl kickstart -k system/com.apple.oahd`. For a backtrace of the
host, send it `SIGABRT` and read its report in `~/Library/Logs/DiagnosticReports`; `sample <host-pid>` profiles the arm64 controller.
Stop testing if translated children hang at launch in process state `U`.

## Layout

| Path | Role |
|---|---|
| `toolchain.cmake` | Included by the top-level CMakeLists before `native/`: shim include directory, `raven_darwin` library, OpenSSL 3, Windows-target clang |
| `darwin.cmake` | Included at the end of `native/CMakeLists.txt`: swaps the guest controller and the bootstrap for the macOS ones |
| `include/` | Linux headers the runtime includes, on top of the macOS facilities they map to; ahead of the SDK on the include path |
| `compat/linux.cpp` | The functions behind those headers (`raven_darwin`, linked into every host target) |
| `compat/sockets.cpp` | `SOCK_SEQPACKET` and abstract names for Unix sockets; defines `send`, `recv`, `sendmsg`, `recvmsg`, `bind`, `connect` |
| `compat/eventfd.cpp` | `eventfd` counters; defines `read`, `write` and `close`, which also tells the socket shim a descriptor is closing |
| `compat/procfs.cpp` | The `/proc` files read as a process API, from libproc, sysctl and Mach |
| `compat/compat_test.cpp` | `darwin_compat_contract`: the interprocess behaviour of the two above |
| `compat/system.cpp` | macOS hardware discovery: CPU, memory, SMBIOS, network interfaces, fonts |
| `compat_test.cpp` | `native_darwin_runtime`: event counters, section offsets, hardware data, the timestamp counter |
| `system_api.cpp`, `desktop_idle.cpp` | Firmware and registry data from the macOS host; the idle budget for backends without a descriptor |
| `cocoa.mm` | The `--desktop=macos` presentation backend |
| `native_process.cpp` | `NativeProcess` for macOS, built instead of `cpu/native_cpu.cpp` |
| `guest/` | The x86_64 guest agent and the zero-fill segment that keeps the low 4 GiB free for Windows images |
| `parallel_test.cpp` | x64/x86 pthread progress, contended atomics, stop wakes, channel reuse, and stream framing contracts |
| `parallel_benchmark.c` | A fixed amount of independent integer work on four Windows threads |
| `bin/raven-clang` | clang for Windows targets, with Homebrew's MinGW-w64 sysroot |

Initial upstream build changes: `CMakeLists.txt` and `native/CMakeLists.txt` (the platform gates and the two includes),
`native/main.cpp` (a `to_string` cast for libc++'s 128-bit file times) and `native/desktop/foreign_surface.cpp` (`__atomic_load_n`
where libc++ has no `atomic_ref<const T>`). Where the runtime reads a `/proc` file with an `ifstream` or lists `/proc`, an `#ifdef __APPLE__`
reads `raven_darwin::proc_stream` or `proc_directory` instead: `nt/child_process.cpp`, `nt/service_api.cpp`, `nt/process_info_api.cpp`
and `nt/winsock_api.cpp`, which also writes guest socket addresses in the macOS layout (length byte, then family byte). `nt/pipe.cpp`
asks `raven_darwin_unix_name_bound` where Linux reads `/proc/net/unix`, and `nt/file_api.cpp` reopens a file with `raven_darwin_reopen`.

## Status

- Phase 1, done: everything builds, and the tests that need no guest process pass.
- The guest controller runs x64 and x86 Windows code. The agent catches its own traps and faults with signal handlers and trades register
  contexts with the host through shared memory. Images, descriptor views, the main stack, anonymous guest heaps and secondary stacks
  have coherent host aliases. Untracked agent mappings still use the task port. Every host access checks the guest's 4 KiB protections first.
- The default scheduler swaps Windows contexts on the agent's main thread. `RAVEN_PARALLEL=1` gives each Windows thread its own Rosetta
  pthread, for both x64 and x86 programs.

## Parallel execution

`RAVEN_PARALLEL=1` applies to every launched program that inherits it. Upstream's `RAVEN_PARALLEL_PROGRAM=<basename.exe>` instead
selects parallel execution by case-insensitive program basename; each child program decides again. It does not change the meaning of
the global switch. Darwin uses the common served-call handlers and decline handshake: decided waits answer in the guest stub, while
blocking parallel waits can park their host frame. Darwin XAudio preserves its outer Windows wait's predicate and deadline across
nested callback execution and retains callback packets until a stopped API frame is available. Parallel GDI presentation still uses
copied pixels until a producer fence is available.

XAudio callbacks currently run from a stopped API frame on the selected Windows thread. A parallel thread that computes or busy-polls
without entering an API can therefore delay them; there is no independent Windows XAudio engine callback thread yet. The parallel XAudio
tests cover callbacks during a timed Windows wait, including preservation of that wait across nested callback execution. They do not
establish Windows' asynchronous delivery guarantee for a computing guest.

The version 4 control block has 256 thread channels and a separate service channel. A channel retains its registers, XSAVE state and stop
reason until the host resumes that pthread; switching host coroutines selects a different channel without copying a running thread's
state. `save_thread(false)` leaves register ownership in the channel, while an explicit stopped snapshot includes the extended state.
Task identities increase even when a channel is reused, so an old scheduler snapshot cannot select a newer pthread.

Each pthread has its own 256 KiB alternate signal stack, TSD base and guest GS base. The signal entry stub derives its channel index from
its position in the array of signal stacks and restores that pthread's TSD before calling C. x86 pthreads keep distinct LDT TEB selectors.
The service thread creates pthreads, targets interrupts with `pthread_kill`, and joins terminated threads before their channels are reused.
An interrupt racing a natural trap leaves that trap's context and reason available to its waiting host frame.

Stops publish their channel with release ordering and write a byte to a nonblocking pipe. `execution_descriptor()` exposes the read end
to the scheduler; draining the pipe consumes notifications, while each channel remains the source of truth about its pending stop.
Address waits still handle individual resume/service handshakes. Host scheduler frames capture their own channel when waiting, so a
different Windows thread can run while a frame is parked.

The existing Linux served-call and queue protocol is used unchanged: each TEB points at its own shared block at offset `0x1ff0`, with
host-recorded ownership for TLS selection and queued graphics calls. The service thread handles memory operations while pthreads run,
and performs locked x86 compare-and-exchange in the translated process so host atomics participate in the guest's memory ordering.
Inherited process handle channels use length-prefixed streams on macOS, preserving frames and `SCM_RIGHTS` across partial reads and exec.
The existing limit of 256 live Windows threads remains. Guest computation can run concurrently; service commands and host API handlers
still run serially. x86 TEB descriptors retain the existing monotonic LDT allocation policy, which can exhaust the table after about 8,000
thread creations in one process even when pthread channels have been reused.

## Anonymous memory aliases

`NativeProcess::map_memory` backs 16 KiB-aligned private reservations with anonymous host VM and remaps those pages into the guest task
through `mach_vm_remap(copy=false)`, as the IOSurface presenter does. Smaller or subpage-aligned allocations use an unlinked descriptor
through the image/stack service, preserving their 4 KiB boundaries. Large private heaps therefore do not charge temporary-file space.
The guest agent still chooses exact, low-address or ordinary
placement and enforces protection on its own 4 KiB pages; the host's 16 KiB mapping is never used as evidence of guest access. The Windows
allocator remains responsible for reserve/commit accounting and reports these regions as `MEM_PRIVATE`. Secondary Windows stacks, TEBs,
PEBs and heaps all use this allocator. The host view is removed on release before the guest address can be reused. Private discard zeros
the shared backing, including pages that the guest cannot currently access.

Reads and writes check the controller's guest protection map before one alias lookup. An absent alias takes the previous task-port or
guest-service path. Aligned scalar reads use acquire loads and scalar writes use release stores while a guest pthread may be running;
locked compare-and-exchange still executes in the guest service thread. Host writes into executable pages also remain guest-service
writes so Rosetta invalidates translated code. A writable-to-executable protection change runs in the guest, and executable discard
reprotects there after zeroing the host view. `RAVEN_DARWIN_DISABLE_PRIVATE_ALIASES=1` selects the old private allocation path for
comparison and fallback testing; remap, file creation or host-view failure selects it automatically for that allocation.
`native_darwin_parallel_contracts` checks the shared path at both guest widths; `native_darwin_private_fallback_contracts` runs the
same concurrent and executable-memory checks with the fallback forced.

Run the contracts serially:

```sh
build/native/native_cpu_test build/native/raven-bootstrap
ctest --test-dir build -j1 --timeout 120 -R 'parallel|queued' --output-on-failure
```

The manual compute benchmark is built with the other Windows fixtures. Compare the same binary in each scheduler mode, after warming
both modes once; keep tests and benchmarks serial:

```sh
env -u RAVEN_PARALLEL build/native/raven --run-process --execution=native --timeout-ms=30000 build/native/darwin-parallel-benchmark.exe
RAVEN_PARALLEL=1 build/native/raven --run-process --execution=native --timeout-ms=30000 build/native/darwin-parallel-benchmark.exe
```

Measured on an M4 Pro with macOS 27.2 on 2026-10-25, with one warmup and five serial runs per mode: the four-thread compute fixture's median
wall time was 2.005 s by default and 0.523 s in parallel (3.83 times faster). The kernel-thread fixture was 0.867 s versus 0.846 s, and heap
stress was 0.401 s versus 0.400 s. Both latter fixtures gained little from parallelism. Times include runtime startup.

On the same configuration, the complete `ctest -j1 --timeout 120` run changed from 194 passed, 58 failed and one skipped (253 tests) to
214 passed, 39 failed and one skipped (254 tests, including the new Darwin contract). All 28 tests matching `parallel|queued` passed;
none of the previously passing tests regressed. The remaining failures were present in the baseline.

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
  Apple's assembler treats a semicolon as a comment, so the host's `isb` and `mrs` must be on separate lines.
- **Across tasks:**
  - From an arm64 task, `mach_vm_read`, `mach_vm_allocate` and `mach_vm_protect` work on the guest.
  - `mach_vm_write` into the guest's executable pages and x86 `thread_get_state` do not.
- **Trap cost:** one trap round trip (signal, host answer, resume) costs about 23 µs. Calls Raven serves through shared memory avoid it.

## Host integration

- Section pixels retain the original host-page-aligned mapping, including any leading slack before a 4 KiB guest offset.
- An allocator query does not report controller-owned images or agent mappings as free just because they are absent from its own table.
- `eventfd` accumulates a 64-bit counter and holds a single FIFO readiness token. Ordinary host `read`, `write` and `close` dispatch through
  the shim; non-event descriptors go to libSystem. Counter overflow, zero writes, blocking/nonblocking reads and semaphore reads are
  supported. Raven's eventfds are local CLOEXEC wake descriptors: duplication, inheritance and POLLOUT overflow readiness are not used
  or emulated. The scheduler drains them; XCB's initial counter stays readable. The Linux CPU controller's eventfd is not built on Darwin.
- CPU identity uses Rosetta's x86 CPUID. Registry brand strings name the Apple host and Rosetta; frequencies use available `hw.cpufrequency*`
  or `hw.perflevel*` values. Apple silicon often publishes no core frequency: zero means unknown, not the 1 GHz timestamp-counter rate.
- SystemConfiguration identifies Wi-Fi even when its link address looks like Ethernet, with interface media as a fallback.
- SMBIOS Type 1 exposes the IOKit model and platform UUID, leaving the serial number empty. Type 17 reports `hw.memsize`. The RSMB firmware
  APIs and installed-memory query use this table.
- Font scans and new prefix Fonts folders include `/System/Library/Fonts`, `/Library/Fonts` and `~/Library/Fonts`. FreeType and Skia load
  all faces of TTC/OTC collections, plus TTF/OTF files. Shell opening uses `/usr/bin/open`.
- Presentation backends without a descriptor receive the scheduler's idle budget; unused time is waited on the scheduler descriptors.

## Controller profiling

On 2026-10-27, fresh copies of `raven-fixtures/firefox-new` with first-run terms accepted, the 4096 MiB commit limit and the launch
recipe's sandbox variables received 30/30 keys in each of two complete runs per mode. Typing median/p95 latencies were 125.28/313.22
and 199.65/432.32 ms by default, versus 1782.81/2201.69 and 1645.52/1856.57 ms in parallel. The median of run medians was therefore
162.47 versus 1714.17 ms (10.55 times slower). Page-ready times were 13.624 and 11.356 s default, 13.931 and 14.212 s parallel.
An intervening default attempt received only 25/30 keys and was excluded; the replacement allowed 1.5 s after raising/focusing and
clicking the window before sending measured keys. All runs used the shared serial slot, `sample` during typing, and both profile switches.

The largest `NativeThreads::pick` branch occupied 2.3-2.4% of default main-thread sample ticks versus 15.8-16.4% in parallel. The largest
reactor-poll branch grew from 0.2-0.5% to 6.6-6.9%, with pending pipe operations repeatedly attempting socket reads. The largest
handle-channel service branch grew from 2.1-2.2% to 9.9-11.0%. These are individual nested branches, not additive totals. Mach guest-memory
read/write fallback had zero ticks in both modes. The samples identify scheduler/IPC polling as investigation targets, but do not prove
that they explain the entire latency gap. No performance change was made from these measurements; parallel remains opt-in.

`RAVEN_PROFILE=1` reports API handler time. `RAVEN_PROFILE_CONTROLLER=1` separately reports memory operations, Mach calls, service commands,
stop waits, served calls and sleep counts. Totals are nested: do not add read time to its Mach-read time, or service time to its parent API.
Stop-wait time includes guest execution and any served handlers; it is not a pure signal-latency measurement.

`RAVEN_DARWIN_SPIN_US` sets the host spin budget, from 0 to 1000 µs (default 60). A short spin keeps served calls off their fallback traps
and avoids sleeping on fast service replies. A service wait always uses the reply value it already checked, preventing a completed reply
from becoming the expected value of a fresh 10 ms sleep. The guest protocol and guest thread channels are unchanged.

```sh
cmake --build build --target darwin_controller_bench
build/native/darwin_controller_bench build/native/raven-bootstrap

profile_dir=$(mktemp -d /tmp/raven-profile-XXXXXX)
cp -R build/testdata/npp "$profile_dir/npp"
RAVEN_PREFIX="$profile_dir/prefix" RAVEN_PROFILE=1 RAVEN_PROFILE_CONTROLLER=1 \
  build/native/raven --run-process --execution=native --timeout-ms=5000 "$profile_dir/npp/notepad++.exe"
```

Notepad++ writes beside itself, so use a private copy. On this revision it reaches window creation and exits with status `0xffffffff`;
startup timing does not establish a working editor session. The `kernel-*.exe` fixtures provide successful end-to-end workloads.
`profiling-results.json` records the measurements and the 0/10/30/60 µs comparison; workload medians exclude a warm-up run. CPU time sums
the host and reaped guest, so it may exceed wall time. Signal timings vary with other work running on the machine.

The Darwin tests cover event counter behavior under concurrent writers, nonzero section offsets, hardware data and fonts, the host
timestamp-counter read, firmware/registry data seen through Windows APIs, untracked mapping queries and Rosetta invalidation after a
shared executable write. Run them and the controller/mapping contracts serially:

```sh
ctest --test-dir build -j1 --timeout 120 --output-on-failure \
  -R 'native_darwin_|native_cpu_contracts|native_shared_mapping_contract|native_compatible_bitmap_contract'
```

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
- File seals are accepted and not enforced.
- `ppoll` waits in milliseconds.

### Graphics, audio and system integration
- **Desktop / Vulkan:** `--desktop=macos` uses Cocoa windows and Metal child views. The Vulkan bridge presents through MoltenVK;
  see [VULKAN.md](VULKAN.md) for verified tests, the desktop integration test, and Direct3D backend limitations.
- **Desktop:** Cocoa lives in `cocoa.mm`; graphics bridge work is separate from the controller.
- **OpenGL:** the bridge uses EGL.
- **Audio:** WinMM, DirectSound, WASAPI, basic XAudio2, PlaySound and authorized microphone capture use CoreAudio; see [audio/IMPLEMENTATION.md](audio/IMPLEMENTATION.md) for coverage and limitations.

## Installed Windows apps in Finder and the Dock

```sh
native/platform/darwin/raven-apps sync                       # all ~/.local/share/raven prefixes
native/platform/darwin/raven-apps sync ~/.local/share/raven/Steam
RAVEN_BINARY="$PWD/build/native/raven" native/platform/darwin/raven-apps sync /path/to/prefix
```

`raven-apps` uses Python 3's standard library, `sips`, `iconutil`, and LaunchServices; it needs no Wine, guest execution, or Python
packages. It reads Start Menu shortcuts from ProgramData and each user's roaming profile, plus each user's Desktop (including
Public). It creates normal, Dock-pinnable application bundles under `~/Applications/Raven`, registers them with LaunchServices for
Finder/Spotlight/Launchpad, and removes its own generated shortcut apps when their shortcuts disappear. Duplicate Desktop/Start Menu
entries for the same target and arguments produce one app. Names come from shortcut filenames (the link's NameString is usually a
long description). Same-name apps from different prefixes receive a suffix. Existing unrelated apps are never replaced or deleted.
Re-running an unchanged sync leaves bundles and their modification times alone.

The parser handles Unicode/ANSI StringData, local LinkInfo (including Unicode paths), filesystem LinkTargetIDLists, relative targets,
working directories, raw Windows arguments, icon indices, and environment target/icon blocks. Drive C maps into `drive_c`, with
case-insensitive component lookup. Uninstallers, help/readme/support entries, URL/non-executable targets, unsupported virtual/network
shell items, other drive letters, and unresolved environment variables are skipped with diagnostics where applicable. It extracts
RT_GROUP_ICON/RT_ICON directly from PE resources (or reads an ICO), then builds an ICNS. Missing icons do not prevent app generation.
Unknown Windows shell extensions, MSI advertised shortcuts and network shares are not resolved. Malformed shortcuts preserve any
previously generated app until a later successful sync.

An app execs the configured Raven binary, preserving LaunchServices' original application record, name, bundle ID and path. On macOS
27.2 this record survives shell → Python → Raven `exec`, even though `NSBundle.mainBundle` does not identify the generated bundle.
`app_identity.mm` applies the saved display name and extracted icon once before Cocoa activation; it does not use private LaunchServices
APIs or change window/input/pump code. `NSProcessInfo.processName` alone is not the bundle identity: launching through the app supplies
the Dock/Cmd-Tab identity and enables pinning. The Finder launcher uses this same path, deriving name/icon from the selected executable.
Both generated apps and Finder launches sync their prefix after the host process exits, using a detached supervisor and a kqueue exit
notification. The supervisor does not access guest files until the host exits. Finder-created apps without shortcuts are retained.
Logs remain in `~/Library/Logs/Raven/<name>.log`; working directory defaults to the target's folder. Shortcut arguments are passed via
`--command-line` verbatim, not split as POSIX shell words. `RAVEN_GUEST_*` settings present during sync are saved in the app's launch
metadata; current launch environment values override them.

`RAVEN_BINARY` selects a build (otherwise `build-macos/native/raven`, then `build/native/raven` in this checkout). Bundles refer to this
checkout's helper and binary, so re-sync after moving either. `RAVEN_APPS_DIR` selects an alternate output folder and `RAVEN_LOG_DIR`
selects logs; both are retained for post-exit sync, including when LaunchServices does not inherit shell environment. A sync only reads
the prefix. Launching a generated app runs Windows code and may write to it; test with a copied prefix. A lock in the output directory
serializes syncs. Parser and lifecycle contracts run without guest execution:

```sh
python3 native/platform/darwin/tests/apps_test.py
```
- **Audio:** WinMM playback uses CoreAudio AudioQueue on macOS; Linux retains PulseAudio. The other macOS Windows audio interfaces share a host mixer and are described in [audio/IMPLEMENTATION.md](audio/IMPLEMENTATION.md).
