# Raven's mtld3d patch

`0001-raven-iosurface-wow64.patch` alters [the pinned zlib-licensed mtld3d source](../../../../upstream/mtld3d/README.md).
It replaces Wine window attachment with Raven's adapter callbacks and IOSurface presentation, translates WoW64
wire pointers and enables unwinding on panic in the unix library. `mtld3d/build_arm64.py` builds this altered source.
