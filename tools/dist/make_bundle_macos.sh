#!/usr/bin/env bash
# Builds the self-contained macOS Apple Silicon bundle: dist/sacred-native-macos-arm64/ and
# dist/sacred-native-macos-arm64.tar.gz.
#   tools/dist/make_bundle_macos.sh [--no-build]      (tools/dist/make_bundle.sh dispatches here on macOS)
# Env: JOBS, DIST_BUILD_DIR (default build/dist), SACRED_FFMPEG_PREFIX (default build/ffmpeg/prefix),
#      SACRED_MACOS_DEPS (default build/macos-deps/prefix),
#      RECIPE (a recipe other than the committed data/recipe.json.gz; local trials only, with BUNDLE_ALLOW_DIRTY=1),
#      BUNDLE_ZIG=0 (leave the fallback compiler out: the player then needs Xcode's command line tools).
#
# The same scheme as the Linux bundle (make_bundle.sh): no code and no data of the game. The runtime (sacred-native,
# built with SACRED_GAME_MODULE), the launcher window (sacred-launcher), the translator with its recipe and headers
# (source/), the Python it runs on (python/: the standard library only) and Zig for Macs without Xcode's command line
# tools (zig/); the launcher builds the game module (libsacred_game.dylib) from the player's own files on the first
# Play, with the system's clang when there is one.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
source tools/dist/bundle_common.sh
name=sacred-native-macos-arm64
out=dist/$name
build="${DIST_BUILD_DIR:-build/dist}"
ffmpeg="${SACRED_FFMPEG_PREFIX:-$root/build/ffmpeg/prefix}"
deps="${SACRED_MACOS_DEPS:-$root/build/macos-deps/prefix}"
recipe="${RECIPE:-$root/data/recipe.json.gz}"
tools=$root/build/dist-tools
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo "make_bundle_macos.sh: macOS arm64 only" >&2
    exit 1
fi
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
if [ ! -s "$recipe" ]; then
    echo "make_bundle_macos.sh: no recipe at $recipe (tools/lift/recipe.py make, or RECIPE=FILE)" >&2
    exit 1
fi

# Pinned third-party tools that travel in the bundle (official release archives, checked by SHA-256).
zig_version=0.17.1
zig_url=https://ziglang.org/download/$zig_version/zig-aarch64-macos-$zig_version.tar.xz
zig_sha=75f1bc7a1e9852f17ec9a0c1895f767c4ec56d46bc8a834920eece1d0359dd8d
python_release=https://github.com/astral-sh/python-build-standalone/releases/download/20261026
python_url="$python_release/cpython-3.13.18%2B20261026-aarch64-apple-darwin-install_only_stripped.tar.gz"
python_sha=8392b6ea51d94391befb2c31ebe75463f045d0b69dfcf5e1bb3234e1f5fa3405
fetch() { # url sha256 -> path of the checked archive in $tools
    local file="$tools/$(basename "${1//%2B/+}")"
    mkdir -p "$tools"
    [ -s "$file" ] || curl -fsSL -o "$file" "$1"
    printf '%s  %s\n' "$2" "$file" | shasum -a 256 --check --status || { echo "checksum mismatch: $file" >&2; exit 1; }
    echo "$file"
}

bundle_require_clean_tree
if [ "${1:-}" != --no-build ]; then
    [ -x "$deps/bin/pkg-config" ] || bash tools/build_macos_deps.sh
    [ -e "$ffmpeg/lib/pkgconfig/libavcodec.pc" ] || bash tools/build_ffmpeg.sh
    # SACRED_GENERATED_DIR points at a directory without shards: the runtime is built alone.
    cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DSACRED_STRICT=ON -DSACRED_BUILD_GAME=ON \
        -DSACRED_GAME_MODULE=ON \
        -DSACRED_RELOCATABLE=ON -DSACRED_QA=OFF -DSACRED_FFMPEG_PREFIX="$ffmpeg" -DSACRED_MACOS_DEPS="$deps" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DSACRED_GENERATED_DIR="$root/$build/no-shards"
    cmake --build "$build" --target sacred-native sacred-launcher --parallel "${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
fi

for exe in sacred-native sacred-launcher; do [ -x "$build/$exe" ] || { echo "missing $build/$exe" >&2; exit 1; }; done

rm -rf "$out" "dist/$name.tar.gz"
mkdir -p "$out/lib" "$out/licenses"
cp "$build/sacred-native" "$build/sacred-launcher" "$out/"
install -m 755 tools/dist/sacred-gold "$out/sacred-gold"
install -d "$out/tools" && install -m 755 tools/legacy_char_pak.py "$out/tools/legacy_char_pak.py"
# The project's wide menu pictures, split: only the pixels that differ from the game's screens, and the hashes of
# the composed pictures (data/menu-backgrounds/composed.sha256), as committed.
bundle_stage_flat "$out/menu-backgrounds" data/menu-backgrounds

# Every non-system dependency is referenced as @rpath/<name>; find each in the build tree or the private prefixes.
search=("$root/$build" "$ffmpeg/lib" "$deps/lib")
locate() { local d; for d in "${search[@]}"; do [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }; done; return 1; }
deps_of() { otool -L "$1" | awk 'NR>1 {print $1}' | grep -v "^$(otool -D "$1" | sed -n 2p)\$" || true; }
# /bin/bash on macOS is 3.2: no associative arrays, so sonames and their sources are parallel indexed arrays.
sonames=() sources=()
bundled() { local s; for s in ${sonames[@]+"${sonames[@]}"}; do [ "$s" = "$1" ] && return 0; done; return 1; }
queue=("$out/sacred-native" "$out/sacred-launcher")
while [ ${#queue[@]} -gt 0 ]; do
    file=${queue[0]}; queue=("${queue[@]:1}")
    while read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/Library/*) continue ;;
            @rpath/*) soname=${dep#@rpath/} ;;
            *) echo "ERROR: $file links a non-relocatable library: $dep" >&2; exit 1 ;;
        esac

        bundled "$soname" && continue
        path=$(locate "$soname") || { echo "ERROR: cannot find $soname (needed by $file)" >&2; exit 1; }
        real=$(realpath "$path")
        install -m 755 "$real" "$out/lib/$soname"
        sonames+=("$soname"); sources+=("$path")
        queue+=("$out/lib/$soname")
    done < <(deps_of "$file")
done

[ "${#sonames[@]}" -gt 0 ] || { echo "no libraries bundled?" >&2; exit 1; }

# Only @executable_path/lib may remain as a run path; then strip and ad-hoc sign (arm64 refuses unsigned code).
rpaths() { otool -l "$1" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'; }
for f in "$out/sacred-native" "$out/sacred-launcher" "$out"/lib/*.dylib; do
    while read -r rp; do
        [ -n "$rp" ] || continue
        case "$f" in "$out/sacred-native"|"$out/sacred-launcher") [ "$rp" = @executable_path/lib ] && continue ;; esac
        install_name_tool -delete_rpath "$rp" "$f"
    done < <(rpaths "$f")
    # The runtime's symbol table is what the game module links against at load time (-undefined dynamic_lookup):
    # strip only debug symbols from it.
    if [ "$f" = "$out/sacred-native" ]; then strip -S "$f"; else strip -x "$f" 2>/dev/null || strip -S "$f"; fi
    codesign --force --sign - "$f"
done

for exe in sacred-native sacred-launcher; do
    if [ "$(rpaths "$out/$exe")" != @executable_path/lib ]; then
        echo "unexpected LC_RPATH of $exe: $(rpaths "$out/$exe")" >&2
        exit 1
    fi
done

nm -gU "$out/sacred-native" | grep ' _recomp_unknown_jump$' >/dev/null ||
    { echo "sacred-native does not export the helpers the game module needs" >&2; exit 1; }

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

# Python: the interpreter and the part of its standard library the translator may use. No package from PyPI, no pip,
# no Tcl/Tk, IDLE, test suites, headers or the shared libpython (the interpreter is linked statically).
python_archive="$(fetch "$python_url" "$python_sha")"
tar -C "$out" -xzf "$python_archive"                          # -> $out/python
py="$out/python" pylib="$out/python/lib/python3.13"
rm -rf "$py/include" "$py/share" "$py"/lib/{pkgconfig,itcl*,tcl*,tk*,thread*,libtcl*,libtk*,libpython*} \
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
# The C compiler for Macs without Xcode's command line tools: only what compiling and linking C for macOS needs
# (Darwin headers and libSystem stubs, clang's headers, compiler_rt and the std it is built from).
if [ "${BUNDLE_ZIG:-1}" != 0 ]; then
zig_archive="$(fetch "$zig_url" "$zig_sha")"
mkdir -p "$out/zig" && tar -C "$out/zig" --strip-components=1 -xJf "$zig_archive"
for d in "$out"/zig/lib/libc/include/*; do
    case "${d##*/}" in any-darwin-any) ;; *) rm -rf "$d" ;; esac
done

rm -rf "$out"/zig/lib/libc/{mingw,musl,wasi,glibc,freebsd,netbsd,openbsd} \
       "$out"/zig/lib/{libtsan,libcxx,libcxxabi,docs,build-web,lldb,init} "$out/zig/doc"
zig_check="$build/zig-check"
mkdir -p "$zig_check"
printf 'extern int helper(int);\nint f(int x) { return helper(x) + 1; }\n' > "$zig_check/t.c"
ZIG_GLOBAL_CACHE_DIR="$zig_check/cache" ZIG_LOCAL_CACHE_DIR="$zig_check/cache" \
    "$out/zig/zig" cc -O1 -fPIC -dynamiclib -undefined dynamic_lookup "$zig_check/t.c" -o "$zig_check/t.dylib" ||
    { echo "the bundled zig cannot build a dylib" >&2; exit 1; }
fi

# Self-containment: no absolute paths outside the OS in any load command, and dyld resolves everything.
for f in "$out/sacred-native" "$out/sacred-launcher" "$out"/lib/*.dylib; do
    if otool -L "$f" | awk 'NR>1 {print $1}' | grep -vE '^(/usr/lib/|/System/Library/|@rpath/)'; then
        echo "ERROR: non-relocatable load command in $f" >&2; exit 1
    fi

    if otool -l "$f" | grep -q "$root"; then echo "ERROR: build path in load commands of $f" >&2; exit 1; fi
    codesign --verify "$f" || { echo "ERROR: invalid signature on $f" >&2; exit 1; }
done
# No shipped binary may need a newer macOS than the bundle promises (a library built without
# MACOSX_DEPLOYMENT_TARGET requires the build machine's version, and dyld then refuses to load it).
minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/ {seen = 1} seen && $1 == "minos" {print $2; exit}'; }
newer_than_target() { # version -> true when it is above $MACOSX_DEPLOYMENT_TARGET
    awk -v have="$1" -v want="$MACOSX_DEPLOYMENT_TARGET" 'BEGIN {
        split(have, h, "."); split(want, w, ".")
        exit !(h[1] + 0 > w[1] + 0 || (h[1] + 0 == w[1] + 0 && h[2] + 0 > w[2] + 0)) }'
}
for f in "$out/sacred-native" "$out/sacred-launcher" "$out"/lib/*.dylib "$out/python/bin/python3" \
         $( [ -d "$out/zig" ] && echo "$out/zig/zig" ); do
    minos=$(minos_of "$f")
    [ -n "$minos" ] || { echo "ERROR: no minimum macOS version in $f" >&2; exit 1; }
    if newer_than_target "$minos"; then
        echo "ERROR: $f needs macOS $minos, the bundle promises $MACOSX_DEPLOYMENT_TARGET" >&2
        exit 1
    fi
done
# Load test: dyld binds every library before main; without --assets the engine prints its usage and exits with 2.
rc=0; env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH DYLD_PRINT_LIBRARIES=1 \
    "$out/sacred-native" --stop-at-winmain >/dev/null 2>"$build/dyld.log" || rc=$?
if [ "$rc" != 2 ]; then
    echo "ERROR: bundled sacred-native failed to start (status $rc):" >&2
    cat "$build/dyld.log" >&2
    exit 1
fi
if grep -q "$root/build" "$build/dyld.log"; then echo "ERROR: dyld loaded a library from $root/build" >&2; exit 1; fi

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
ffsrc=build/ffmpeg/src depsrc=build/macos-deps/src
for f in COPYING.LGPLv2.1 LICENSE.md; do [ -f "$ffsrc/$f" ] && cp "$ffsrc/$f" "$lic/ffmpeg-$f"; done
# LGPL: the source of the bundled FFmpeg libraries and its build script travel with them.
bundle_stage_ffmpeg_source "$ffmpeg" "$lic"
[ -f "$depsrc/sdl2/LICENSE.txt" ] && cp "$depsrc/sdl2/LICENSE.txt" "$lic/SDL2-LICENSE.txt"
[ -f "$depsrc/freetype/LICENSE.TXT" ] && cp "$depsrc/freetype/LICENSE.TXT" "$lic/freetype-LICENSE.TXT"
[ -f "$depsrc/freetype/docs/FTL.TXT" ] && cp "$depsrc/freetype/docs/FTL.TXT" "$lic/freetype-FTL.TXT"
[ -d "$out/zig" ] && cp "$out/zig/LICENSE" "$lic/zig-LICENSE"
cp "$out/python/lib/python3.13/LICENSE.txt" "$lic/python-LICENSE.txt"

# License completeness: every bundled library must be covered by a license text shipped in the bundle.
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

for soname in "${sonames[@]}"; do
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

[ "$missing_license" = 0 ] || { echo "make_bundle_macos.sh: bundle license check failed" >&2; exit 1; }

# Nothing of the game: no generated C, no export, no game file names besides the ones the runtime looks up.
if find "$out" -name 'sacred_[0-9]*.c' -o -name '*.jsonl.gz' -o -iname 'Sacred.exe' | grep . >/dev/null; then
    echo "make_bundle_macos.sh: game-derived files in the bundle" >&2; exit 1
fi

cp tools/dist/macos/BUNDLE_README.md "$out/README.md"
{ echo "Bundled shared libraries ($(date -u +%F)):"; for i in "${!sonames[@]}"; do
      printf '  %-32s %s\n' "${sonames[$i]}" "$(bundle_source_name "${sources[$i]}" "$(realpath "${sources[$i]}")")"
  done | sort
  python_version=$(basename "$python_url" | sed 's/%2B/+/' | cut -d- -f2)
  echo "Tools: CPython $python_version (standard library only)$( [ -d "$out/zig" ] && echo ", zig $zig_version")"; } \
    > "$out/lib/BUNDLED_LIBRARIES.txt"

# bsdtar: deterministic ownership; exclude macOS metadata (._* files, xattrs).
COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname wheel --no-xattrs -C dist -czf "dist/$name.tar.gz" "$name"
du -sh "$out" "dist/$name.tar.gz"
bundle_verify "dist/$name.tar.gz" "$name"
echo "BUNDLE_OK dist/$name.tar.gz"
