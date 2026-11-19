#!/usr/bin/env bash
# Fresh-clone setup. Every product lands under build/ or analysis/export/ (both gitignored); nothing is written into
# tracked files.
#   SACRED_ASSETS=/path/to/"Sacred Gold" tools/bootstrap.sh [--venv] [--ffmpeg] [--export] [--generate] [--all]
# Default (no flag): --venv --ffmpeg --export --generate, each step skipped when its product already exists.
#   --venv      Python venv in build/venv (pefile, capstone, unicorn, pytest; tests/cpu/requirements.txt)
#   --ffmpeg    pinned minimal LGPL FFmpeg in build/ffmpeg/prefix (downloads the source tarball, checks its SHA-256)
#   --export    Ghidra IR export of the player's own Sacred.exe, granny.dll and tincat2.dll into analysis/export/
#               (needs Ghidra 12.1.4 headless in GHIDRA_HOME;
#               a missing ghidra-proj/sacred project is created by importing and analysing Sacred.exe)
#   --generate  C shards from the export into $BUILD_DIR/generated (BUILD_DIR default build/game)
# An export you already have can be reused: place it in analysis/export (or set SACRED_EXPORT_DIR) and the step is
# skipped.
# By default the C is generated from the recipe data/recipe.json.gz (tools/lift/recipe.py) and the player's files:
# no Ghidra is needed and --export is skipped. SACRED_RECIPE=FILE names another recipe; SACRED_RECIPE= (empty) uses
# the export instead.
set -euo pipefail
cd "$(dirname "$0")/.."
steps=("$@")
[ ${#steps[@]} -gt 0 ] && [ "${steps[0]}" != --all ] || steps=(--venv --ffmpeg --export --generate)
want() { local s; for s in "${steps[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
build_dir="${BUILD_DIR:-build/game}"
export_dir="${SACRED_EXPORT_DIR:-$PWD/analysis/export}"
python="$PWD/build/venv/bin/python"
# Absolute, since generate.py runs with the repository root as working directory like this script.
export SACRED_RECIPE="${SACRED_RECIPE-$PWD/data/recipe.json.gz}"
if [ -n "$SACRED_RECIPE" ]; then
    SACRED_RECIPE="$(cd "$(dirname "$SACRED_RECIPE")" && pwd)/$(basename "$SACRED_RECIPE")"
    export SACRED_RECIPE
fi
need_assets() { : "${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"; }

if want --venv && [ ! -x "$python" ]; then
    python3 -m venv build/venv
    build/venv/bin/pip install --quiet -r tests/cpu/requirements.txt
fi

if want --ffmpeg && [ ! -e build/ffmpeg/prefix/lib/pkgconfig/libavcodec.pc ] &&
    [ -z "${SACRED_FFMPEG_PREFIX:-}" ]; then
    bash tools/build_ffmpeg.sh
fi

if want --export && [ -n "${SACRED_RECIPE:-}" ]; then
    echo "SACRED_RECIPE set: no export needed"
elif want --export; then
    if [ -s "$export_dir/sacred-startup/instructions.jsonl.gz" ] && [ -s "$export_dir/granny/instructions.jsonl.gz" ] &&
       [ -s "$export_dir/tincat2/instructions.jsonl.gz" ]; then
        echo "export present in $export_dir"
    else
        need_assets
        headless="${GHIDRA_HOME:?set GHIDRA_HOME to a Ghidra 12.1.4 installation}/support/analyzeHeadless"
        [ -x "$headless" ] || { echo "Ghidra 12.1.4 headless not found at $headless (set GHIDRA_HOME)" >&2; exit 1; }
        if [ ! -f ghidra-proj/sacred.gpr ]; then
            mkdir -p ghidra-proj analysis/export/import-home analysis/export/import-tmp
            HOME="$PWD/analysis/export/import-home" XDG_CONFIG_HOME="$PWD/analysis/export/import-home/.config" \
            _JAVA_OPTIONS="-Xmx3g -Duser.home=$PWD/analysis/export/import-home \
-Djava.io.tmpdir=$PWD/analysis/export/import-tmp" \
                "$headless" "$PWD/ghidra-proj" sacred -import "$SACRED_ASSETS/Sacred.exe" -max-cpu 2 \
                -analysisTimeoutPerFile 7200
        fi

        bash tools/lift/export_exe.sh
        bash tools/lift/export_dlls.sh
    fi

    if [ ! -s "$export_dir/instructions.jsonl.gz" ] && [ "$export_dir" = "$PWD/analysis/export" ]; then
        need_assets
        bash tools/lift/export_baseline.sh
    fi
fi

if want --generate; then
    # The resolution site table is a generator input that changes more often than the export: regenerate when it
    # (or the generator) differs from the recorded inputs of the present shards.
    if command -v sha256sum >/dev/null; then sum=(sha256sum); else sum=(shasum -a 256); fi
    inputs=$(cat tools/lift/generate.py tools/lift/recipe.py ${SACRED_RECIPE:+"$SACRED_RECIPE"} \
        tools/lift/resolution.py tools/lift/resolution_sites.json tools/lift/x87_fast.py tools/lift/promote.py \
        third_party/recomp-kit/tools/recomp/*.py | "${sum[@]}" | cut -c1-64)
    recorded=$(cat "$build_dir/generated/.inputs" 2>/dev/null || true)
    if [ -f "$build_dir/generated/sources.cmake" ] && [ "$recorded" = "$inputs" ]; then
        echo "generated shards present in $build_dir/generated"
    else
        need_assets
        [ -x "$python" ] || { echo "run tools/bootstrap.sh --venv first" >&2; exit 1; }
        mkdir -p "$build_dir"
        SACRED_GENERATED_DIR="$PWD/$build_dir/generated" SACRED_EXPORT_DIR="$export_dir" \
            "$python" tools/lift/generate.py
        printf '%s\n' "$inputs" > "$build_dir/generated/.inputs"
    fi
fi
