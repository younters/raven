# x87sidecar for macOS

The optional [x87sidecar](https://github.com/athei/x87sidecar/tree/v1.9.0) component uses release v1.9.0.
The archive SHA-256 is `104098b0ba8baed37d4d63cf62ab3b55493889b8fe9ee225cad8febcd6bc1e78`, checked by
[`provision-x87sidecar.py`](../../platform/darwin/scripts/provision-x87sidecar.py).

`native/platform/darwin/guest/coop_proto.h` carries the v1.9.0 cooperative handshake declarations and their MIT
license notice, copyright Lifeisawful. The provisioner installs the sidecar binary. See the
[sidecar setup](../../platform/darwin/x87sidecar.md) for runtime selection.
