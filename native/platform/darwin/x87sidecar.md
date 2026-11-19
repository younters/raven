# Cooperative x87 acceleration

Run `python3 scripts/provision-x87sidecar.py` on Apple Silicon. It installs
x87sidecar v1.9.0's flat, non-entitled binary under
`~/Documents/Projects/raven-components/x87sidecar/`, outside the checkout.
The release archive SHA256 is
`104098b0ba8baed37d4d63cf62ab3b55493889b8fe9ee225cad8febcd6bc1e78`.
Run the installed binary with `--probe` after a macOS update.

PE32 processes automatically use that provisioned path. Set
`RAVEN_X87_SIDECAR=/absolute/path/to/x87sidecar` to override it, or set
`RAVEN_X87_SIDECAR=` to disable it. PE64 processes bypass the sidecar.
This avoids disabling Rosetta's AOT cache and paying startup IPC for code
which normally uses SSE. CreateProcess children go through the same
NativeProcess/GuestAgent launch path and choose by their own image width.

The host spawns `[x87sidecar, --cooperative, raven-bootstrap, service]`.
x87sidecar preserves that PID across exec, sets `ROSETTA_DISABLE_AOT` and
publishes `x87sidecar.<pid>`. At the very beginning of bootstrap main, before
Raven's signal handlers, memory service or guest code, the bootstrap sends
its task and current thread control ports to that service. It blocks for
the reply and calls `sys_icache_invalidate` on its own thread for both
returned patch ranges. This works in the x86_64 Rosetta process containing
32-bit LDT code; no separate i386 macOS process exists.

No entitled binary, debugger, ptrace or privileged attach is used. Missing
provisioning leaves the ordinary launch unchanged. A bad explicit path or
spawn error falls back to a direct bootstrap launch. Unsupported Rosetta
and hook setup failures use the sidecar's cooperative fallback. Its stderr
is inherited by Raven, preserving the `RUNNING WITHOUT X87 ACCELERATION`
banner in captured traces. `RAVEN_TRACE_API=1` additionally records the
number of patched ranges invalidated; a successful hook returns two.
All `X87_*` configuration variables except a stale handshake service are
forwarded from Raven's environment. Raven does not change upstream's
arithmetic or fault-handling defaults.

The upstream [README](https://github.com/athei/x87sidecar/blob/v1.9.0/README.md)
and [internals](https://github.com/athei/x87sidecar/blob/v1.9.0/docs/internals.md)
document precision and fault differences. Arithmetic uses binary64, so
24/64-bit precision modes, exponent extremes and some transcendentals can
differ from stock Rosetta. The fixture deliberately uses PC=53 and exact
arithmetic plus sin(0), accumulating 2,000,000 results into a known checksum
of 8,000,000.0. Bit identity here does not establish game-wide equivalence.

CMake registers `darwin_x87sidecar_*` only when its `RAVEN_X87_SIDECAR`
file exists and both MinGW compilers are available. Build `raven`,
`raven-bootstrap` and `darwin_x87sidecar_fixtures` with `-j8`, then run:

```
/tmp/raven-ctest-lock.sh x87sidecar ctest --test-dir build-x87 \
  -R '^darwin_x87sidecar_' --stop-on-failure -V
```

The checks compare the fixed checksum with direct execution, require both
hook ranges and no fallback banner, test PE64 bypass and a PE32 CreateProcess
parent/child pair, and exercise missing, unsupported and partially installed
hook rollback paths. Guest QPC timing excludes launch and IPC setup; speedup
is reported without a threshold. Stop after any timeout and inspect for exec
state U before further x86 launches. Never run this bootstrap under a debugger.

`guest/coop_proto.h` is copied unchanged (apart from the license preamble)
from x87sidecar v1.9.0 with its MIT notice. The implementation follows the
cooperative handshake in athei/wine's `cx-26-patched` loader. The matching
wine-build policy is documented at commit
`2f021ac89ae684e06214c924ee5f161526e69637` (current main has since moved to FEX).

## Validation on Darwin 27.2.0 (arm64 T6041)

The flat v1.9.0 `--probe` returned 0 and `supported` (runtime and
libRosettaRuntime checks passed, both hook sites found, FEAT_AFP).
`darwin_x87sidecar_correctness` returned checksum `415e848000000000`
bit for bit on both paths, with two patched ranges and no fallback banner:
0.431694 s stock, 0.016906 s cooperative, 25.53x. The CreateProcess child
also matched, with two handshake records for parent and child:
0.431912 s stock, 0.017382 s cooperative, 24.85x. These are individual
microbenchmark samples, not game speedups or performance assertions.

Build commands (all succeeded):

```
cmake --build build-x87 --target raven raven-bootstrap darwin_x87sidecar_fixtures -j8
cmake --build build-x87 --target native_cpu_test native_x86_test -j8
```

Passing final test commands:

```
/tmp/raven-ctest-lock.sh x87-release-check ctest --test-dir build-x87 -R '^darwin_x87sidecar_' --stop-on-failure -V
/tmp/raven-ctest-lock.sh x87-contracts env RAVEN_X87_SIDECAR= ctest --test-dir build-x87 -R '^native_(cpu_contracts|x86_contracts)$' --stop-on-failure --output-on-failure
```

Four integration tests passed together; both existing contracts passed.

The first two development runs exposed harness issues (the absent-provision
HOME was not writable, and the child flag matched the executable filename).
Both were corrected; all four integration cases and both existing contracts
passed in the subsequent targeted runs. No launch timed out or entered an
exec hang. Configuring with `-DRAVEN_X87_SIDECAR=/nonexistent/raven-x87sidecar`
then running locked `ctest -N -R '^darwin_x87sidecar_'` registered zero tests;
the provisioned configuration was restored afterwards. The fallback case
also ran successfully with a fresh HOME and no opt-in environment variable,
without an acceleration warning.
