# Raven's patches to FEX

These patches apply to dbc-hbin/FEX-darwin at `370c63057fc9fc826b58fe15d59930876df55b5f`
(https://github.com/dbc-hbin/FEX-darwin), a Darwin port of FEX (https://github.com/FEX-Emu/FEX). `scripts/build-fex-modules.py`
applies them in order and builds `xtajit64.dll` and `xtajit.dll`; `scripts/fex-modhost-pins.json` records each patch's SHA-256.

FEX and FEX-darwin's own changes are MIT-licensed, each with its own notice. The patches modify those files and are offered under
the same MIT terms. The licence texts of the pinned source and its dependencies (SoftFloat, xxHash and others) are in its
`Darwin/` licence bundle, which the build copies beside the modules; keep them with any copy of the modules.
