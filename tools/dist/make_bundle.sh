#!/usr/bin/env bash
# Builds the self-contained Linux ARM64 bundle: dist/sacred-native-linux-arm64/ and
# dist/sacred-native-linux-arm64.tar.gz.
#   tools/dist/make_bundle.sh [--no-build]
# Env: JOBS, DIST_BUILD_DIR (default build/dist), SACRED_FFMPEG_PREFIX (default build/ffmpeg/prefix),
#      RECIPE (a recipe other than the committed data/recipe.json.gz; local trials only, with BUNDLE_ALLOW_DIRTY=1),
#      BUNDLE_ZIG=0 (leave the fallback compiler out: the player then needs cc, gcc or clang).
#
# The bundle holds no code and no data of the game. It carries the runtime (sacred-native, built with
# SACRED_GAME_MODULE: it loads the game code from a library), the launcher window (sacred-launcher), the translator
# with its recipe and the headers the generated code needs (source/), the Python it runs on (python/: the standard
# library only, the translator reads PE files and decodes x86 itself) and a C compiler for machines without one (zig/).
# On the player's machine the launcher builds the game module from the player's own Sacred.exe, granny.dll and
# tincat2.dll (tools/launcher/build_game.py) with the system's compiler, or Zig, and then starts the game.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
source tools/dist/bundle_common.sh
name=sacred-native-linux-arm64
out=dist/$name
build="${DIST_BUILD_DIR:-build/dist}"
ffmpeg="${SACRED_FFMPEG_PREFIX:-$root/build/ffmpeg/prefix}"
recipe="${RECIPE:-$root/data/recipe.json.gz}"
tools=$root/build/dist-tools
[ "$(uname -s)" = Darwin ] && exec bash tools/dist/make_bundle_macos.sh "$@"
if [ "$(uname -m)" != aarch64 ] || [ "$(uname -s)" != Linux ]; then
    echo "make_bundle.sh: Linux aarch64 or macOS arm64 only" >&2
    exit 1
fi
if [ ! -s "$recipe" ]; then
    echo "make_bundle.sh: no recipe at $recipe (tools/lift/recipe.py make, or RECIPE=FILE)" >&2
    exit 1
fi

# Pinned third-party tools that travel in the bundle (official release archives, checked by SHA-256).
zig_version=0.17.1
zig_url=https://ziglang.org/download/$zig_version/zig-aarch64-linux-$zig_version.tar.xz
zig_sha=a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795
python_release=https://github.com/astral-sh/python-build-standalone/releases/download/20261026
python_url="$python_release/cpython-3.13.18%2B20261026-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz"
python_sha=578d9da1983463ffb0c8c98ab0da2f18177f44069328eb0af847860c1eb05c3e
fetch() { # url sha256 -> path of the checked archive in $tools
    local file="$tools/$(basename "${1//%2B/+}")"
    mkdir -p "$tools"
    [ -s "$file" ] || curl -fsSL -o "$file" "$1"
    printf '%s  %s\n' "$2" "$file" | sha256sum --check --status || { echo "checksum mismatch: $file" >&2; exit 1; }
    echo "$file"
}

bundle_require_clean_tree
if [ "${1:-}" != --no-build ]; then
    [ -e "$ffmpeg/lib/pkgconfig/libavcodec.pc" ] || bash tools/build_ffmpeg.sh
    [ -e build/linux-deps/prefix/lib/pkgconfig/sdl2.pc ] || bash tools/build_linux_deps.sh
    # SACRED_GENERATED_DIR points at a directory without shards: the runtime is built alone.
    cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DSACRED_STRICT=ON -DSACRED_BUILD_GAME=ON \
        -DSACRED_GAME_MODULE=ON \
        -DSACRED_RELOCATABLE=ON -DSACRED_QA=OFF -DSACRED_FFMPEG_PREFIX="$ffmpeg" \
        -DSACRED_GENERATED_DIR="$root/$build/no-shards" \
        $( [ -d "$build" ] || echo -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ )
    cmake --build "$build" --target sacred-native sacred-launcher --parallel "${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
fi

for exe in sacred-native sacred-launcher; do [ -x "$build/$exe" ] || { echo "missing $build/$exe" >&2; exit 1; }; done

# Libraries that stay external: guaranteed by any glibc desktop distribution, or loaded through the GPU/windowing/audio
# stack that must match the user's drivers and display server (see README).
baseline='^(linux-vdso|ld-linux-aarch64|libc|libm|libdl|libpthread|librt|libmvec|libstdc\+\+|libgcc_s|libz|libzstd'
baseline+='|libbz2|libexpat|libpng16|libbrotli(common|dec|enc)|libffi'
baseline+='|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libGLESv2|libgbm|libdrm|libX[A-Za-z0-9-]*|libxcb[A-Za-z0-9-]*'
# libXdmcp's own dependencies (Debian/Ubuntu): present wherever the X libraries are.
baseline+='|libbsd|libmd'
baseline+='|libwayland-[a-z-]+|libxkbcommon|libdecor-0'
baseline+='|libasound|libpulse|libpulsecommon-[0-9.]+|libsamplerate|libsndfile|libFLAC|libvorbis|libvorbisenc|libopus'
baseline+='|libogg|libmpg123|libmp3lame'
baseline+='|libasyncns|libdbus-1|libsystemd|libapparmor|libcap|libgcrypt|libgpg-error|liblz4|liblzma|libcom_err)\.so'

rm -rf "$out" "dist/$name.tar.gz"
mkdir -p "$out/lib" "$out/licenses"
for exe in sacred-native sacred-launcher; do
    cp "$build/$exe" "$out/$exe"
    # The runtime's symbol table is what the game module links against at load time: keep the dynamic symbols.
    strip --strip-unneeded "$out/$exe"
done

install -m 755 tools/dist/sacred-gold "$out/sacred-gold"
# The project's logo (the windows' icon, platform/app_icon.h, and the desktop entry's) and the desktop entry, which
# install-desktop-entry.sh adds to the player's application menu on request (nothing is installed by default).
install -d "$out/icons" "$out/desktop"
install -m 644 data/icons/sacred-native-256.png data/icons/sacred-native-512.png "$out/icons/"
install -m 644 tools/dist/linux/sacred-native.desktop "$out/desktop/sacred-native.desktop"
install -m 755 tools/dist/linux/install-desktop-entry.sh "$out/install-desktop-entry.sh"
if command -v desktop-file-validate >/dev/null; then
    sed 's|^Exec=@LAUNCHER@$|Exec=sacred-launcher|' tools/dist/linux/sacred-native.desktop \
        > "$build/sacred-native.desktop"
    desktop-file-validate "$build/sacred-native.desktop"
fi
install -d "$out/tools" && install -m 755 tools/legacy_char_pak.py "$out/tools/legacy_char_pak.py"

declare -A source_of
for exe in sacred-native sacred-launcher; do
    while read -r soname arrow path _; do
        [ "$arrow" = "=>" ] && [ -f "$path" ] || continue
        [[ "$soname" =~ $baseline ]] && continue
        [ -n "${source_of[$soname]:-}" ] && continue
        real="$(readlink -f "$path")"
        install -m 755 "$real" "$out/lib/$soname"
        strip --strip-unneeded "$out/lib/$soname" 2>/dev/null || true
        source_of[$soname]="$path"
    done < <(LD_LIBRARY_PATH="$root/$build:$ffmpeg/lib:$root/build/linux-deps/prefix/lib" ldd "$build/$exe")
done

[ "${#source_of[@]}" -gt 0 ] || { echo "no libraries bundled?" >&2; exit 1; }

# The translator and what the generated code compiles against (our GPL-3.0-or-later code, the vendored recomp-kit,
# Bochs/SoftFloat
# headers), plus the recipe: addresses and lengths of the game's instructions, no byte of the game.
src="$out/source"
bundle_stage "$src" tools/lift/generate.py tools/lift/recipe.py tools/lift/resolution.py \
      tools/lift/resolution_sites.json \
      tools/lift/x87_fast.py tools/lift/promote.py tools/launcher/build_game.py \
      third_party/recomp-kit/tools/game_config.py ':(glob)third_party/recomp-kit/tools/recomp/*.py' \
      ':(glob)third_party/recomp-kit/runtime/*.h' ':(glob)runtime/cpu/*.h' runtime/modules/game_module.h \
      runtime/modules/resolution_table.h runtime/sacred/native.c runtime/granny/native.c data/entry-targets.json \
      data/entry-cpuid.json third_party/bochs/config.h ':(glob)third_party/bochs/softfloat3e/include/**/*.h'
bundle_stage_recipe "$recipe" "$src/recipe.json.gz"
# The project's wide menu pictures, split: only the pixels that differ from the game's screens, and the hashes of
# the composed pictures (data/menu-backgrounds/composed.sha256), as committed.
bundle_stage_flat "$out/menu-backgrounds" data/menu-backgrounds

# Python: the interpreter and the part of its standard library the translator may use. No package from PyPI, no pip,
# no Tcl/Tk, IDLE, test suites, headers or the shared libpython (the interpreter is linked statically).
python_archive="$(fetch "$python_url" "$python_sha")"
tar -C "$out" -xzf "$python_archive"                          # -> $out/python
py="$out/python" pylib="$out/python/lib/python3.13"
rm -rf "$py/include" "$py/share" "$py"/lib/{pkgconfig,itcl*,tcl*,tk*,thread*,libtcl*,libpython*} \
       "$py"/bin/{idle*,pip*,pydoc*,python3*-config} \
       "$pylib"/{site-packages,ensurepip,idlelib,tkinter,turtledemo,turtle.py,pydoc_data,venv,test,unittest/test} \
       "$pylib"/{lib2to3,__phello__,_pyrepl,config-3.13-*,sqlite3,curses,dbm,xmlrpc,wsgiref,http,email,mailbox.py} \
       "$pylib"/lib-dynload/{_tkinter,_dbm,_curses,_curses_panel}*
find "$py" -name '__pycache__' -prune -exec rm -rf {} +
mkdir -p "$pylib/site-packages"
"$py/bin/python3" -I -B - "$src" <<'PY' || { echo "bundled Python cannot load the translator" >&2; exit 1; }
import sys
root = sys.argv[1]
sys.path[:0] = [root + "/tools/lift", root + "/tools/launcher", root + "/third_party/recomp-kit/tools/recomp"]
import generate, build_game, translate, pe, x86dec  # noqa: F401
PY
# The C compiler for machines without one.
if [ "${BUNDLE_ZIG:-1}" != 0 ]; then
zig_archive="$(fetch "$zig_url" "$zig_sha")"
mkdir -p "$out/zig" && tar -C "$out/zig" --strip-components=1 -xJf "$zig_archive"
# Only what compiling C for this machine needs: the aarch64 glibc headers and stubs, clang's headers, compiler_rt (and
# the std it is built from); not the other targets' libcs, C++ or sanitizer sources, docs or debugger scripts
# (400 MB -> about 220 MB; checked by compiling and linking a shard with an empty cache).
for d in "$out"/zig/lib/libc/include/*; do
    case "${d##*/}" in aarch64-linux-gnu|aarch64-linux-any|any-linux-any|generic-glibc) ;; *) rm -rf "$d" ;; esac
done

rm -rf "$out"/zig/lib/libc/{mingw,musl,wasi,darwin,freebsd,netbsd,openbsd} \
       "$out"/zig/lib/{libtsan,libcxx,libcxxabi,docs,build-web,lldb,init} "$out/zig/doc"
"$out/zig/zig" cc --version >/dev/null
fi

# Self-containment checks: only $ORIGIN/lib in RUNPATH, no build paths anywhere, nothing unresolved.
for exe in sacred-native sacred-launcher; do
    runpath="$(readelf -d "$out/$exe" | sed -n 's/.*\(RUNPATH\|RPATH\).*\[\(.*\)\]/\2/p')"
    [ "$runpath" = '$ORIGIN/lib' ] || { echo "unexpected RUNPATH of $exe: '$runpath'" >&2; exit 1; }
    if env -u LD_LIBRARY_PATH ldd "$out/$exe" | grep 'not found' >/dev/null; then
        echo "unresolved libraries of $exe:" >&2; env -u LD_LIBRARY_PATH ldd "$out/$exe" | grep 'not found' >&2; exit 1
    fi

    if env -u LD_LIBRARY_PATH ldd "$out/$exe" | grep "$root/build" >/dev/null; then
        echo "ldd of $exe references $root/build" >&2
        exit 1
    fi
done

for f in "$out"/lib/*; do
    if readelf -d "$f" 2>/dev/null | grep -E 'RUNPATH|RPATH' | grep "$root" >/dev/null; then
        echo "build path in $f" >&2
        exit 1
    fi
done

nm -D --defined-only "$out/sacred-native" | grep ' recomp_unknown_jump$' >/dev/null ||
    { echo "sacred-native does not export the helpers the game module needs" >&2; exit 1; }

# Licenses and notices.
lic="$out/licenses"
cp LICENSE THIRD_PARTY_NOTICES.md "$lic/"
cp third_party/bochs/COPYING "$lic/bochs-fpu-LGPL-2.1.txt"
cp third_party/bochs/softfloat3e/COPYING.txt "$lic/softfloat3e-BSD-3.txt"
cp third_party/minimp3/LICENSE "$lic/minimp3-LICENSE"
cp third_party/recomp-kit/LICENSE "$lic/recomp-kit-LICENSE"
cp third_party/recomp-kit/NOTICE "$lic/recomp-kit-NOTICE"
for d in bochs minimp3 dr_libs stb recomp-kit; do cp "third_party/$d/UPSTREAM.json" "$lic/$d-UPSTREAM.json"; done
cp third_party/dr_libs/ADMISSION.md "$lic/dr_wav-ADMISSION.md"
sed -n '/^\/\*/,$p' third_party/dr_libs/dr_wav.h | sed -n '/ALTERNATIVE 1/,$p' > "$lic/dr_wav-LICENSE" || true
sed -n '/^This software is available under 2 licenses/,$p' third_party/stb/stb_image.h > "$lic/stb-LICENSE" || true
ffsrc=build/ffmpeg/src depsrc=build/linux-deps/src
if [ ! -f "$ffsrc/LICENSE.md" ]; then
    # The pinned FFmpeg source (tools/build_ffmpeg.sh) is gone after a clean: take its license texts from the tarball.
    ffver="$(sed -n 's/^version=//p' tools/build_ffmpeg.sh)" ffsum="$(sed -n 's/^sha256=//p' tools/build_ffmpeg.sh)"
    ffarchive="$(fetch "https://ffmpeg.org/releases/ffmpeg-$ffver.tar.xz" "$ffsum")"
    ffsrc="$tools/ffmpeg-$ffver"
    mkdir -p "$ffsrc"
    tar -C "$ffsrc" --strip-components=1 -xJf "$ffarchive" "ffmpeg-$ffver/COPYING.LGPLv2.1" "ffmpeg-$ffver/LICENSE.md"
fi

for f in COPYING.LGPLv2.1 LICENSE.md; do [ -f "$ffsrc/$f" ] && cp "$ffsrc/$f" "$lic/ffmpeg-$f"; done
# LGPL: the source of the bundled FFmpeg libraries and its build script travel with them.
bundle_stage_ffmpeg_source "$ffmpeg" "$lic"
[ -f "$depsrc/sdl2/LICENSE.txt" ] && cp "$depsrc/sdl2/LICENSE.txt" "$lic/SDL2-LICENSE.txt"
[ -f "$depsrc/freetype/LICENSE.TXT" ] && cp "$depsrc/freetype/LICENSE.TXT" "$lic/freetype-LICENSE.TXT"
[ -f "$depsrc/freetype/docs/FTL.TXT" ] && cp "$depsrc/freetype/docs/FTL.TXT" "$lic/freetype-FTL.TXT"
[ -d "$out/zig" ] && cp "$out/zig/LICENSE" "$lic/zig-LICENSE"
cp "$out/python/lib/python3.13/LICENSE.txt" "$lic/python-LICENSE.txt"

missing_license=0
require_license() {
    if [ ! -s "$lic/$1" ]; then
        echo "ERROR: bundle lacks license file licenses/$1 ($2)" >&2
        missing_license=1
    fi
}
for f in LICENSE THIRD_PARTY_NOTICES.md bochs-fpu-LGPL-2.1.txt softfloat3e-BSD-3.txt minimp3-LICENSE \
         dr_wav-LICENSE stb-LICENSE \
         recomp-kit-LICENSE recomp-kit-NOTICE python-LICENSE.txt $( [ -d "$out/zig" ] && echo zig-LICENSE ); do
    require_license "$f" "project/vendored source or bundled tool"
done

for soname in "${!source_of[@]}"; do
    case "$soname" in
        libav* | libsw*)
            require_license ffmpeg-COPYING.LGPLv2.1 "$soname (FFmpeg LGPL)"
            require_license ffmpeg-LICENSE.md "$soname (FFmpeg)"
            ;;
        libsacred_softfloat*) ;;   # Bochs LGPL + SoftFloat BSD-3, required above
        libSDL2*) require_license SDL2-LICENSE.txt "$soname" ;;
        libfreetype*) require_license freetype-LICENSE.TXT "$soname"; require_license freetype-FTL.TXT "$soname" ;;
        *) echo "ERROR: bundled library $soname has no license mapping" >&2; missing_license=1 ;;
    esac
done

[ "$missing_license" = 0 ] || { echo "make_bundle.sh: bundle license check failed" >&2; exit 1; }

# Nothing of the game: no generated C, no export, no game file names besides the ones the runtime looks up.
if find "$out" -name 'sacred_[0-9]*.c' -o -name '*.jsonl.gz' -o -iname 'Sacred.exe' | grep . >/dev/null; then
    echo "make_bundle.sh: game-derived files in the bundle" >&2; exit 1
fi

cp tools/dist/BUNDLE_README.md "$out/README.md"
{ echo "Bundled shared libraries ($(date -u +%F)):"; for s in $(printf '%s\n' "${!source_of[@]}" | sort); do
      printf '  %-28s %s\n' "$s" "$(bundle_source_name "${source_of[$s]}" "$(readlink -f "${source_of[$s]}")")"
  done
  python_version=$(basename "$python_url" | sed 's/%2B/+/' | cut -d- -f2)
  echo "Tools: CPython $python_version (standard library only)$( [ -d "$out/zig" ] && echo ", zig $zig_version")"; } \
    > "$out/lib/BUNDLED_LIBRARIES.txt"

tar --sort=name --owner=0 --group=0 --numeric-owner -C dist -czf "dist/$name.tar.gz" "$name"
du -sh "$out" "dist/$name.tar.gz"
bundle_verify "dist/$name.tar.gz" "$name"
echo "BUNDLE_OK dist/$name.tar.gz"
