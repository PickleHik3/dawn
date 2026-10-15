#!/bin/bash
# Cross-build this checkout for Termux x86_64 (the Android emulator), mirroring tlstore/recipes/cross/build-dawn.sh.
# Usage: TL_SYSROOT=/path/sysroot-x86_64 tests/ux/emu/build-x86_64.sh   (run from the repo root)
set -euo pipefail
S=${TL_OUT:-$PWD/build-emu}; mkdir -p "$S"
SRC=${DAWN_SRC:-$PWD}
NDK=${TL_NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}
SYSROOT=${TL_SYSROOT:?set TL_SYSROOT to an x86_64 Termux sysroot (tlstore/recipes/cross/termux-sysroot.sh with TL_TERMUX_ARCH=x86_64)}
PREFIX=/data/data/com.termux/files/usr
P=$SYSROOT$PREFIX
B=$S/build-x86_64
VER="0.1.3+$(git -C $SRC rev-parse --short HEAD)"
PKG_CONFIG_SYSROOT_DIR="$SYSROOT" PKG_CONFIG_LIBDIR="$P/lib/pkgconfig" \
cmake -S "$SRC" -B "$B" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release -DDAWN_VERSION="$VER" -DUSE_LIBAI=ON \
    -DCMAKE_FIND_ROOT_PATH="$P" \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DCMAKE_C_FLAGS="-I$P/include" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$P/lib -Wl,-rpath,$PREFIX/lib"
cmake --build "$B" -j"$(nproc)" --target dawn
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" -o "$S/dawn-x86_64" "$B/dawn"
for m in ']52;c;?' '/chat/completions' 'replace_selection' '<append_to_note>'; do grep -qa "$m" "$S/dawn-x86_64" || { echo "missing marker $m"; exit 1; }; done
grep -qa xclip "$S/dawn-x86_64" && { echo "xclip present"; exit 1; }
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$S/dawn-x86_64" | grep -E 'NEEDED|RUNPATH'
echo "Built: $S/dawn-x86_64 ($VER)"
