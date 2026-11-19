#!/usr/bin/env bash
# Linux: builds SDL2 and FreeType from pinned tarballs into a private prefix, the same versions as on macOS
# (tools/build_macos_deps.sh), so the runtime and its bundle do not depend on the distribution's packages.
#   tools/build_linux_deps.sh            -> build/linux-deps/prefix (SDL2, FreeType); CMake prefers this prefix
# Needs the development headers SDL2's back ends compile against (X11, Wayland, xkbcommon, libdecor, PulseAudio,
# ALSA, PipeWire, EGL/GL, drm/gbm) and zlib; SDL2 loads those libraries at run time (dlopen), so the bundle carries
# none of them. FFmpeg is built separately by tools/build_ffmpeg.sh. Each step is skipped when its product exists.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo "build_linux_deps.sh: Linux only" >&2; exit 1; }
mkdir -p build/linux-deps
# The physical directory: build/ may be a link, and the compiler names the sources by their real path, which is the
# one -ffile-prefix-map below has to match.
root=$(cd build/linux-deps && pwd -P)
dl=$root/dl src=$root/src prefix=$root/prefix
mkdir -p "$dl" "$src" "$prefix"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
# Source paths (__FILE__, debug info) relative to the build root: no path of the build machine in a shipped library.
cmake_common=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_INSTALL_LIBDIR=lib
    -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_RPATH='$ORIGIN')

fetch() { # name url sha256 -> unpacked source directory in $src/name
    local name=$1 url=$2 sum=$3 file=$dl/${2##*/}
    [ -s "$file" ] || curl -fsSL -o "$file" "$url"
    printf '%s  %s\n' "$sum" "$file" | sha256sum --check --status || { echo "checksum mismatch: $file" >&2; exit 1; }
    rm -rf "${src:?}/$name"; mkdir -p "$src/$name"
    tar -C "$src/$name" --strip-components=1 -xf "$file"
}

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
    # Back ends the game needs; each one's client library is opened at run time.
    cmake -S "$src/sdl2" -B "$src/sdl2/build" "${cmake_common[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON -DSDL_PIPEWIRE=ON -DSDL_KMSDRM=OFF \
        -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_ALSA_SHARED=ON \
        -DSDL_PIPEWIRE_SHARED=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_NAS=OFF -DSDL_ARTS=OFF
    cmake --build "$src/sdl2/build" --parallel "$jobs" && cmake --install "$src/sdl2/build"
fi
# SDL2's pkg-config file asks for an rpath to the prefix; the runtime's RUNPATH must stay $ORIGIN/lib (dev builds get
# the prefix through CMake's build rpath).
sed -i -E 's/ ?-Wl,-rpath,[^ ]+//g; s/ ?-Wl,--enable-new-dtags//g' "$prefix/lib/pkgconfig/sdl2.pc"
echo "LINUX_DEPS_OK $prefix"
