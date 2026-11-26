# Optional x87 acceleration

[x87sidecar](../../upstream/x87sidecar/README.md) patches Apple's Rosetta runtime in memory to accelerate x87 arithmetic.
It applies only to 32-bit Windows programs under Rosetta. Raven can launch these programs without the sidecar. Its
arithmetic can differ from stock Rosetta.

To select the sidecar explicitly:

```sh
sidecar="$PWD/../raven-components/x87sidecar"
python3 native/platform/darwin/scripts/provision-x87sidecar.py --output "$sidecar"
export RAVEN_X87_SIDECAR="$sidecar/x87sidecar"
```

The sidecar is opt-in: with `RAVEN_X87_SIDECAR` unset or empty, Raven does not look for it. `--output` is required.
A missing executable or failure to spawn it leaves the ordinary bootstrap path available.
The bootstrap and sidecar use a cooperative task-port handshake before Windows code starts.

CMake registers `darwin_x87sidecar_*` when the `RAVEN_X87_SIDECAR` cache path names an existing sidecar and both MinGW
compilers are available. Build the `darwin_x87sidecar_fixtures` target to run these tests.
