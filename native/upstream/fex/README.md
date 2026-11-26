# FEX for macOS

Raven's FEX modules are built from [FEX-darwin](https://github.com/dbc-hbin/FEX-darwin) at
`370c63057fc9fc826b58fe15d59930876df55b5f`, a Darwin port of [FEX](https://github.com/FEX-Emu/FEX).
Both projects carry MIT notices. The provisioner copies the pinned source's license bundle, including dependency notices,
beside the modules. The build toolchain is [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), under Apache-2.0 with
the LLVM exception for LLVM components. The source, toolchain, patch and output hashes are in
[fex-modhost-pins.json](../../platform/darwin/fex/scripts/fex-modhost-pins.json).

Raven stores the hosted module patch series under
[native/platform/darwin/fex/patches](../../platform/darwin/fex/patches). The separate substrate build uses the patch
under `native/platform/darwin/fex/modhost/patches`. See the [FEX build instructions](../../platform/darwin/fex/README.md).
