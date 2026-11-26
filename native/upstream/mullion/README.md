# Mullion for macOS

The experimental Direct3D 12 backend uses [Mullion](https://github.com/dttdrv/mullion) at
`46e6f0508c4d48dd1aec3ac22df4da87cad102a8`, licensed under LGPL-2.1-or-later.
`native/platform/darwin/mullion/build.py` applies `raven-adapter.patch` from the same directory.
The patch adapts the native calls to the Raven adapter. The build copies the upstream license and
records artifact hashes.
