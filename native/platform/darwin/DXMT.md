# DXMT on macOS

DXMT translates Direct3D 10 and 11 to Metal. Its Windows DLLs use a Wine-style unix-call table to reach the native
library `winemetal.so`. Under Rosetta, Raven loads the x86_64 library in its guest bootstrap and switches to the
pthread's Darwin stack and TLS for each call. Under FEX, Raven calls an arm64 build of the library from the host.
The Cocoa desktop presents the resulting IOSurfaces.

## Provisioning

The [DXMT component record](../../upstream/dxmt/README.md) covers the release and source pins. By default,
`setup.sh` provisions the prebuilt v0.80 PE DLLs, rewrites the native library's Wine dependencies to Raven's adapter
and signs the altered library. The Rosetta agent requires the unix-call table contract `raven_unix_get_contract`.
The default release library and the plain `RAVEN_DXMT_SOURCE_BUILD=1` output both lack that contract.

For Rosetta, use `dxmt/build_checked.py` to rebuild the unix side from a configured source build. It needs the source
directory and Meson build directory produced by source provisioning, plus a new output directory for the checked
library, adapter and call count. Use all three outputs as the `unix64` directory of a component with the matching
source build's `pe64` directory. The configured or runtime `RAVEN_DXMT_ROOT` points to that component. For FEX, use
[the matching arm64 overlay](GRAPHICS.md#fex-graphics). The plain source provisioner also builds the x64
`dcomp-metal.dll`.

The provisioner is [`scripts/provision-dxmt.py`](scripts/provision-dxmt.py), and the release pin is in
[`dxmt.json`](dxmt.json). DXMT loads `d3dcompiler_47.dll` from System32, which resolves to Raven's own shader compiler;
the provisioner installs no compiler. A source build (`RAVEN_DXMT_SOURCE_BUILD=1`) needs `RAVEN_DXMT_BUILD_ROOT` to name
an external directory for its tools. The arm64 checked rebuild takes the LLVM 15 prefix from `--source-llvm`,
`RAVEN_DXMT_LLVM15` or `RAVEN_DXMT_BUILD_ROOT`.

## Presentation

The Windows DLLs and unix library must agree on the call table layout. The Rosetta gate accepts registered libraries
and bounds-checks calls against that table. It preserves the guest's Win64 nonvolatile registers. Native work signals
the scheduler through a completion queue.

DirectComposition binds a stream to a guest window using the `RAVENDCOMPSWAPCHAIN` window property. Guest code in
the same process can set that property, which opts its stream into composition binding. It is a binding marker.

A controller forwards an IOSurface stream to another Raven process over Mach messages. The receiver reads the kernel
audit trailer and accepts a stream only from a controller with the same executable path and effective user whose
reply service is registered in the same prefix. A stream on screen is bound to its sender's audit PID and PID version.
A release must match the PID, slot and sequence recorded when the frame was forwarded, and a retire must match the
displayed stream, window and original sender. Malformed or rejected messages are destroyed along with their rights.
Mixed installations have no compatibility contract, and a queued release can be dropped if the receiving controller
exits first.

## Tests

`darwin_unix_call` and `darwin_unix_call_parallel` exercise the Rosetta call gate. With a matching DXMT component
configured, `darwin_dxmt_*` covers device creation, drawing, shared textures and presentation. The fixtures check
results returned to the guest and, for presentation tests, rendered pixels. `darwin_dcomp_local`, `darwin_dcomp_foreign`
and `darwin_dcomp_foreign-hidpi` cover local and cross-process DirectComposition and need a window server and screen
capture; check for an actual pass, because the image harness can exit with a skip.
