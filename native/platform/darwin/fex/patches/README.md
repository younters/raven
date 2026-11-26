# Raven's FEX patches

These patches adapt the pinned FEX-darwin source for Raven's ARM64EC and WoW64 module host, including the host callback
ABI, fault handling, page tracking, and code cache. They modify MIT-licensed files and retain those terms.
[`build-fex-modules.py`](../scripts/build-fex-modules.py) `--raven-host` applies the series in filename order.

See [FEX provenance](../../../../upstream/fex/README.md) for the source pin and notices.
