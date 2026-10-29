# Sourced by setup.sh for the opt-in source overlay. Toolchains stay outside the checkout.
# vkd3d (the System32 d3dcompiler_47's HLSL compiler) builds from git: autotools, missing formulae only.
dxmt_missing=
for formula in autoconf automake libtool; do
    brew list --formula "$formula" >/dev/null 2>&1 || dxmt_missing="$dxmt_missing $formula"
done
# shellcheck disable=SC2086
[ -z "$dxmt_missing" ] || HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install $dxmt_missing
dxmt_build_root=${RAVEN_DXMT_BUILD_ROOT:-$HOME/Documents/Projects/raven-components-build}
dxmt_llvm_default=$dxmt_build_root/llvm15

dxmt_x64_llvm() {
    [ -f "$1/lib/libLLVMCore.a" ] &&
        /usr/bin/lipo -verify_arch x86_64 "$1/lib/libLLVMCore.a" >/dev/null 2>&1 &&
        /usr/bin/grep -q '#define LLVM_VERSION_MAJOR 15' "$1/include/llvm/Config/llvm-config.h"
}

if [ -z "${RAVEN_DXMT_LLVM15:-}" ]; then
    if dxmt_x64_llvm "$dxmt_llvm_default"; then
        RAVEN_DXMT_LLVM15=$dxmt_llvm_default
    else
        # Homebrew may still offer LLVM 15, but arm64-only archives cannot link into the Rosetta unixlib.
        if brew info --formula llvm@15 >/dev/null 2>&1; then
            brew list --formula llvm@15 >/dev/null 2>&1 ||
                HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install llvm@15
            dxmt_brew_llvm=$(brew --prefix llvm@15)
            if dxmt_x64_llvm "$dxmt_brew_llvm"; then RAVEN_DXMT_LLVM15=$dxmt_brew_llvm; fi
        fi
        if [ -z "${RAVEN_DXMT_LLVM15:-}" ]; then
            [ ! -e "$dxmt_llvm_default" ] || {
                echo "setup.sh: incompatible LLVM exists at $dxmt_llvm_default; choose RAVEN_DXMT_BUILD_ROOT" >&2
                exit 1
            }
            dxmt_llvm_source=$dxmt_build_root/llvm15-source
            dxmt_llvm_build=$dxmt_build_root/llvm15-build
            mkdir -p "$dxmt_build_root"
            if [ ! -d "$dxmt_llvm_source/.git" ]; then
                git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git "$dxmt_llvm_source"
            fi
            [ "$(git -C "$dxmt_llvm_source" rev-parse HEAD)" = 8dfdcc7b7bf66834a761bd8de445840ef68e4d1a ] || {
                echo "setup.sh: LLVM source is not the pinned 15.0.7 commit" >&2; exit 1;
            }
            cmake -S "$dxmt_llvm_source/llvm" -B "$dxmt_llvm_build" -G Ninja \
                -DCMAKE_INSTALL_PREFIX="$dxmt_llvm_default" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
                -DLLVM_HOST_TRIPLE=x86_64-apple-darwin -DLLVM_ENABLE_ASSERTIONS=ON \
                -DLLVM_ENABLE_ZSTD=OFF -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD= \
                -DLLVM_BUILD_TOOLS=OFF -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=OFF
            cmake --build "$dxmt_llvm_build" -j4
            cmake --install "$dxmt_llvm_build"
            RAVEN_DXMT_LLVM15=$dxmt_llvm_default
        fi
    fi
fi
dxmt_x64_llvm "$RAVEN_DXMT_LLVM15" || {
    echo "setup.sh: RAVEN_DXMT_LLVM15 must contain x86_64 LLVM 15 headers and static libraries" >&2
    exit 1
}

RAVEN_DXMT_WINE_BUILD=${RAVEN_DXMT_WINE_BUILD:-$dxmt_build_root/dxmt-wine}
mkdir -p "$RAVEN_DXMT_WINE_BUILD"
if [ ! -f "$RAVEN_DXMT_WINE_BUILD/Makefile" ]; then
    (cd "$RAVEN_DXMT_WINE_BUILD" &&
        CC='clang -arch x86_64' CXX='clang++ -arch x86_64' "$COMPONENTS/tools/wine-src/configure" \
        --host=x86_64-apple-darwin --enable-win64 --disable-tests --without-x --without-freetype \
        --without-gstreamer --without-pcap --without-sdl --without-usb --without-vulkan --without-cups --without-gnutls)
fi
make -C "$RAVEN_DXMT_WINE_BUILD" -j4 BISON="$(brew --prefix bison)/bin/bison" \
    tools/winebuild/winebuild dlls/ntdll/x86_64-windows/libntdll.a \
    dlls/dbghelp/x86_64-windows/libdbghelp.a dlls/winecrt0/x86_64-windows/libwinecrt0.a
