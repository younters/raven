#!/bin/sh
# Sets up Raven on an Apple silicon Mac in one go: Homebrew packages, the guest components the scripts/provision-*.py tools build, a
# CPU Skia with the Liberation fonts for text, and a configured build directory. Everything it downloads or builds goes outside the
# source tree. Run it again to pick up what is missing; finished steps are skipped.
#
#   native/platform/darwin/setup.sh [build-directory]
#
# RAVEN_COMPONENTS (default: ../raven-components beside the checkout) and RAVEN_SKIA (default: ../raven-skia) choose where the
# components and Skia go. The Vulkan loader and MoltenVK come from the LunarG SDK (https://vulkan.lunarg.com), installed separately.
set -eu

SOURCE=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD=${1:-$SOURCE/build-macos}
COMPONENTS=${RAVEN_COMPONENTS:-$(dirname "$SOURCE")/raven-components}
SKIA=${RAVEN_SKIA:-$(dirname "$SOURCE")/raven-skia}
# The pinned revisions: native/upstream/skia/README.md names Skia's, and Liberation 2.1.5 provides the three regular fonts.
SKIA_REVISION=a21aae83d6e4e3da284100a30f0a5eafc443b60f
WINE_VERSION=10.0
LIBERATION_URL=https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz

step() { printf '\n==> %s\n' "$1"; }

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "setup.sh: this is for Apple silicon Macs" >&2; exit 1; }
command -v brew >/dev/null || { echo "setup.sh: Homebrew is required (https://brew.sh)" >&2; exit 1; }
/usr/bin/pgrep -q oahd || softwareupdate --install-rosetta --agree-to-license

step "Homebrew packages"
# Only what is missing: installing a formula Homebrew already has would upgrade it and everything that depends on it.
missing=
for formula in cmake ninja python mingw-w64 lld llvm openssl@3 libpng freetype pixman libxkbcommon bison zstd; do
	brew list --formula "$formula" >/dev/null 2>&1 || missing="$missing $formula"
done
# shellcheck disable=SC2086
[ -z "$missing" ] || HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install $missing
export PATH="$COMPONENTS/tools/bin:$(brew --prefix bison)/bin:$PATH"
mkdir -p "$COMPONENTS/downloads" "$COMPONENTS/tools/bin"

# widl, the IDL compiler the ReactOS components need, is not packaged for macOS: Wine's own is built from its source.
if [ ! -x "$COMPONENTS/tools/bin/widl" ]; then
	step "widl from Wine $WINE_VERSION"
	archive="$COMPONENTS/downloads/wine-$WINE_VERSION.tar.xz"
	[ -f "$archive" ] || curl -fL -o "$archive" "https://dl.winehq.org/wine/source/10.0/wine-$WINE_VERSION.tar.xz"
	rm -rf "$COMPONENTS/tools/wine-src" "$COMPONENTS/tools/wine-build"
	mkdir -p "$COMPONENTS/tools/wine-src" "$COMPONENTS/tools/wine-build"
	tar -xf "$archive" -C "$COMPONENTS/tools/wine-src" --strip-components 1
	(cd "$COMPONENTS/tools/wine-build" && ../wine-src/configure --enable-win64 --disable-tests --without-x --without-freetype \
		--without-gstreamer --without-pcap --without-sdl --without-usb --without-vulkan --without-cups --without-gnutls >/dev/null &&
		make -j"$(sysctl -n hw.ncpu)" tools/widl/widl >/dev/null)
	cp "$COMPONENTS/tools/wine-build/tools/widl/widl" "$COMPONENTS/tools/bin/widl"
fi

provision() { # name, then the provisioner and its arguments
	name=$1
	shift
	if [ -e "$COMPONENTS/$name/.provisioned" ]; then return; fi
	step "component $name"
	python3 "$SOURCE/scripts/$@"
	touch "$COMPONENTS/$name/.provisioned"
}
provision rtl provision-rtl.py --output "$COMPONENTS/rtl"
provision text provision-text.py --output "$COMPONENTS/text"
provision controls provision-controls.py --output "$COMPONENTS/controls"
provision controls32 provision-controls.py --arch x86 --output "$COMPONENTS/controls32"
provision installer provision-installer.py --output "$COMPONENTS/installer"
provision installer32 provision-installer.py --arch x86 --output "$COMPONENTS/installer32"
provision roots provision-roots.py --output "$COMPONENTS/roots"
provision libpng provision-libpng.py --output "$COMPONENTS/libpng"
provision graphics provision-graphics.py --destination "$COMPONENTS/graphics"

# Skia for the text of GDI, as native/upstream/skia/README.md qualifies it: CPU only, the custom empty font manager, system FreeType.
if [ ! -f "$SKIA/out/libskia.a" ]; then
	step "Skia $SKIA_REVISION"
	if [ ! -d "$SKIA/src/.git" ]; then
		mkdir -p "$SKIA/src"
		git -C "$SKIA/src" init -q
		git -C "$SKIA/src" remote add origin https://skia.googlesource.com/skia.git
	fi
	git -C "$SKIA/src" fetch -q --depth 1 origin "$SKIA_REVISION"
	git -C "$SKIA/src" checkout -q FETCH_HEAD
	(cd "$SKIA/src" && python3 bin/fetch-gn >/dev/null)
	"$SKIA/src/bin/gn" gen "$SKIA/out" --root="$SKIA/src" --args="is_official_build=true is_debug=false target_cpu=\"arm64\" \
skia_use_partition_alloc=false skia_enable_ganesh=false skia_enable_graphite=false skia_use_gl=false skia_use_metal=false \
skia_use_vulkan=false skia_use_fonthost_mac=false skia_use_freetype=true skia_use_system_freetype2=true \
skia_enable_fontmgr_custom_empty=true skia_enable_fontmgr_empty=false skia_use_fontconfig=false skia_use_harfbuzz=false \
skia_use_icu=false skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_libpng_decode=false \
skia_use_libpng_encode=false skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_zlib=false skia_use_expat=false \
skia_use_piex=false skia_use_dng_sdk=false skia_use_wuffs=false skia_use_xps=false skia_enable_pdf=false skia_enable_skshaper=false \
skia_enable_skunicode=false skia_enable_tools=false extra_cflags=[\"-I$(brew --prefix)/include/freetype2\"]"
	ninja -C "$SKIA/out" skia
fi

# The Liberation release's TTF files differ from the build the upstream hashes qualify, so their own hashes are what the build checks.
if [ ! -f "$COMPONENTS/fonts/LiberationSans-Regular.ttf" ]; then
	step "Liberation fonts"
	work=$(mktemp -d)
	curl -fL -o "$work/liberation.tar.gz" "$LIBERATION_URL"
	tar -xzf "$work/liberation.tar.gz" -C "$work"
	mkdir -p "$COMPONENTS/fonts"
	for file in LiberationSans-Regular.ttf LiberationMono-Regular.ttf LiberationSerif-Regular.ttf LICENSE; do
		cp "$work/liberation-fonts-ttf-2.1.5/$file" "$COMPONENTS/fonts/"
	done
	rm -rf "$work"
fi
font_hash() { shasum -a 256 "$COMPONENTS/fonts/$1" | cut -d' ' -f1; }

step "configure $BUILD"
cmake -S "$SOURCE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DRAVEN_SKIA_SOURCE="$SKIA/src" -DRAVEN_SKIA_LIBRARY="$SKIA/out/libskia.a" -DRAVEN_FONT_DIRECTORY="$COMPONENTS/fonts" \
	"-DFONT_HASHES=$(font_hash LiberationSans-Regular.ttf);$(font_hash LiberationMono-Regular.ttf);$(font_hash LiberationSerif-Regular.ttf)" \
	-DRAVEN_RTL_COMPONENT_PATH="$COMPONENTS/rtl/raven-rtl.dll" -DRAVEN_TEXT_COMPONENT_PATH="$COMPONENTS/text/raven-text.dll" \
	-DRAVEN_CONTROLS_COMPONENT_DIR="$COMPONENTS/controls" -DRAVEN_CONTROLS32_COMPONENT_DIR="$COMPONENTS/controls32" \
	-DRAVEN_INSTALLER_COMPONENT_DIR="$COMPONENTS/installer" -DRAVEN_INSTALLER32_COMPONENT_DIR="$COMPONENTS/installer32" \
	-DRAVEN_ROOT_CERTIFICATE_DIRECTORY="$COMPONENTS/roots" \
	-DRAVEN_DXVK_PE64_DIR="$COMPONENTS/graphics/dxvk/pe64" -DRAVEN_DXVK_PE32_DIR="$COMPONENTS/graphics/dxvk/pe32" \
	-DRAVEN_VKD3D_PE64_DIR="$COMPONENTS/graphics/vkd3d-proton/pe64" \
	-DPNG_LIBRARY_RELEASE="$COMPONENTS/libpng/install/lib/libpng16.a" -DPNG_PNG_INCLUDE_DIR="$COMPONENTS/libpng/install/include"

step "build"
cmake --build "$BUILD"

printf '\nDone. Run a program with:\n  RAVEN_PREFIX=~/.local/share/raven/my-app %s/native/raven --run-process --execution=native --timeout-ms=86400000 --desktop=macos program.exe\n' "$BUILD"
