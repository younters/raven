# Raven's patch to mtld3d

`0001-raven-iosurface-wow64.patch` alters athei/mtld3d v0.15.1 at `9919759fa7c03362deac1b985db6d93de9e33ecf`
(https://github.com/athei/mtld3d), copyright Alexander Theissen, under the zlib licence. As that licence asks, this marks the
altered source: the patch replaces Wine window attachment with Raven's adapter callbacks and IOSurface presentation, translates WoW64
wire pointers, and makes the unix half unwind on panic. It is not the original software. `mtld3d/build_arm64.py` builds the patched
source and copies the upstream `LICENSE` beside its output. The Cargo dependencies keep their own licences.
