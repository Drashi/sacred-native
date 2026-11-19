#!/usr/bin/env bash
# Builds the pinned, minimal, LGPL-only FFmpeg that the movie adapter links dynamically (docs/notes/movie-playback.md).
# Output: build/ffmpeg/prefix (shared libraries + headers + pkg-config files). Only the ASF demuxer, the WMV/WMA
# decoders, swscale and swresample are enabled; no GPL code, no external libraries (--disable-autodetect), no network.
# Used on both Linux and macOS: distribution FFmpeg packages (Ubuntu's, Homebrew's) are GPL-configured and drag in
# unrelated libraries. On macOS the libraries are identified as @rpath/<name> (dev builds and the bundle set the rpath).
# FFmpeg compiles its configure line into the libraries (avutil_configuration), so it is configured for the neutral
# prefix /ffmpeg and installed through DESTDIR: no path of the build machine ends up in a shipped library.
set -euo pipefail
cd "$(dirname "$0")/.."
# macOS: the deployment target of the runtime and of tools/build_macos_deps.sh. Without it the libraries require the
# build machine's own macOS version and the bundle does not start on an older one.
[ "$(uname -s)" != Darwin ] || export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
version=8.0.1
sha256=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
root=$PWD/build/ffmpeg
dl=$root/dl src=$root/src build=$root/build prefix=$root/prefix
mkdir -p "$dl"
tarball=$dl/ffmpeg-$version.tar.xz
[ -s "$tarball" ] || curl -fsSL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
if command -v sha256sum >/dev/null; then printf '%s  %s\n' "$sha256" "$tarball" | sha256sum --check -
else printf '%s  %s\n' "$sha256" "$tarball" | shasum -a 256 --check -; fi
rm -rf "$src" "$build"; mkdir -p "$src" "$build"
tar -C "$src" --strip-components=1 -xf "$tarball"
cd "$build"
# Configured through a relative path, so source file names (__FILE__) are relative too.
"../${src##*/}/configure" --prefix=/ffmpeg --enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-network --disable-autodetect \
    --disable-everything --enable-protocol=file --enable-demuxer=asf \
    --enable-decoder=wmv1,wmv2,wmv3,wmav1,wmav2,wmapro --enable-swscale --enable-swresample \
    --extra-version=sacred-native-minimal
make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
rm -rf "$root/stage" "$prefix"
make install DESTDIR="$root/stage"
mkdir -p "$(dirname "$prefix")" && mv "$root/stage/ffmpeg" "$prefix" && rm -rf "$root/stage"
# The .pc files name the configured prefix; point them at the real one for this build.
sed -i.orig "s|=/ffmpeg|=$prefix|" "$prefix"/lib/pkgconfig/*.pc && rm -f "$prefix"/lib/pkgconfig/*.pc.orig
grep -q '^#define FFMPEG_LICENSE "LGPL' "$build/config.h" || { echo 'FFmpeg build is not LGPL' >&2; exit 1; }
if [ "$(uname -s)" = Darwin ]; then
    for lib in "$prefix"/lib/*.dylib; do
        [ -L "$lib" ] && continue
        id=$(otool -D "$lib" | sed -n 2p)
        install_name_tool -id "@rpath/${id##*/}" "$lib"
        otool -L "$lib" | awk 'NR>1 {print $1}' | while read -r dep; do
            case "$dep" in
                /ffmpeg/lib/* | "$prefix"/lib/*) install_name_tool -change "$dep" "@rpath/${dep##*/}" "$lib" ;;
            esac
        done

        codesign --force --sign - "$lib"
    done
fi

echo "FFMPEG_MINIMAL_OK $prefix"
