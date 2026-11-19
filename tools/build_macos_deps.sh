#!/usr/bin/env bash
# macOS Apple Silicon: builds every host dependency that the macOS SDK lacks into a private prefix, so nothing is
# installed system-wide (no Homebrew packages are required or used).
#   tools/build_macos_deps.sh            -> build/macos-deps/prefix (bin/pkg-config, SDL2, FreeType)
# The SDK supplies OpenGL and zlib. Fonts are matched, XML read and written and SHA-256 computed by our own code.
# FFmpeg is built separately by tools/build_ffmpeg.sh (pinned LGPL configuration, same on Linux and macOS).
# Every tarball is pinned by SHA-256; each step is skipped when its product already exists.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "build_macos_deps.sh: macOS arm64 only" >&2; exit 1; }
root=$PWD/build/macos-deps
dl=$root/dl src=$root/src prefix=$root/prefix
mkdir -p "$dl" "$src" "$prefix"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
# Only our own prefix is visible to pkg-config and CMake: never pick up Homebrew libpng, brotli, harfbuzz, ...
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH=""
export PATH="$prefix/bin:$PATH"
# Source paths (__FILE__, debug info) relative to the build root: no path of the build machine in a shipped library.
cmake_common=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DCMAKE_FIND_FRAMEWORK=LAST
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DCMAKE_INSTALL_NAME_DIR=@rpath -DBUILD_SHARED_LIBS=ON)

fetch() { # name url sha256 -> unpacked source directory in $src/name
    local name=$1 url=$2 sum=$3 file=$dl/${2##*/}
    [ -s "$file" ] || curl -fsSL -o "$file" "$url"
    if ! printf '%s  %s\n' "$sum" "$file" | shasum -a 256 --check --status; then
        echo "checksum mismatch: $file" >&2
        exit 1
    fi
    rm -rf "${src:?}/$name"; mkdir -p "$src/$name"
    tar -C "$src/$name" --strip-components=1 -xf "$file"
}

if [ ! -x "$prefix/bin/pkg-config" ]; then
    fetch pkgconf https://distfiles.ariadne.space/pkgconf/pkgconf-2.3.0.tar.xz \
        3a9080ac51d03615e7c1910a0a2a8df08424892b5f13b0628a204d3fcce0ea8b
    (cd "$src/pkgconf" && ./configure --prefix="$prefix" --disable-static \
        --with-pkg-config-dir="$prefix/lib/pkgconfig" --with-system-libdir=/usr/lib \
        --with-system-includedir=/usr/include &&
     make -j"$jobs" && make install)
    ln -sf pkgconf "$prefix/bin/pkg-config"
fi

# The SDK ships zlib without a pkg-config file; describe it so that freetype2.pc resolves.
mkdir -p "$prefix/lib/pkgconfig"
for pc in "zlib 1.2.12 z"; do
    set -- $pc
    [ -e "$prefix/lib/pkgconfig/$1.pc" ] ||
        printf 'Name: %s\nDescription: %s from the macOS SDK\nVersion: %s\nLibs: -l%s\nCflags:\n' "$1" "$1" "$2" "$3" \
            > "$prefix/lib/pkgconfig/$1.pc"
done

if [ ! -e "$prefix/lib/pkgconfig/freetype2.pc" ]; then
    fetch freetype https://download-mirror.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz \
        0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    cmake -S "$src/freetype" -B "$src/freetype/build" "${cmake_common[@]}" -DFT_REQUIRE_ZLIB=ON \
        -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "$src/freetype/build" --parallel "$jobs" && cmake --install "$src/freetype/build"
fi

if [ ! -e "$prefix/lib/pkgconfig/sdl2.pc" ]; then
    fetch sdl2 https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
        5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    cmake -S "$src/sdl2" -B "$src/sdl2/build" "${cmake_common[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
    cmake --build "$src/sdl2/build" --parallel "$jobs" && cmake --install "$src/sdl2/build"
fi

# Shared libraries are identified as @rpath/<name> so that dev builds (rpath -> prefix/lib) and the bundle
# (rpath -> @executable_path/lib) load the same files.
for lib in "$prefix"/lib/*.dylib; do
    [ -L "$lib" ] && continue
    id=$(otool -D "$lib" | sed -n 2p)
    case "$id" in @rpath/*) ;; *) install_name_tool -id "@rpath/${id##*/}" "$lib" ;; esac
    otool -L "$lib" | awk 'NR>1 {print $1}' | while read -r dep; do
        case "$dep" in "$prefix"/lib/*) install_name_tool -change "$dep" "@rpath/${dep##*/}" "$lib" ;; esac
    done

    codesign --force --sign - "$lib" 2>/dev/null
done

echo "MACOS_DEPS_OK $prefix"
