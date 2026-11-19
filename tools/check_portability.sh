#!/usr/bin/env bash
# macOS-portability gate (docs/ARCHITECTURE.md, "Portability rules").
# Compiles runtime/ + platform/ and a sample of generated shards with clang and
# gcc using strict ISO modes and -Werror, then runs the host-API scan.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
out=build/portability
mkdir -p "$out"
command -v clang >/dev/null || { echo 'clang is required' >&2; exit 1; }
# Compilers to qualify: clang and GNU gcc. On macOS `gcc` is Apple clang under another name; there only clang
# is checked unless a real GCC is named with PORTABILITY_GCC (e.g. a private gcc-15 build).
compilers="clang gcc"
if ! gcc --version 2>/dev/null | grep -q 'Free Software Foundation'; then
    if [ -n "${PORTABILITY_GCC:-}" ]; then
        compilers="clang $PORTABILITY_GCC"
    else
        compilers=clang
        echo 'note: no GNU gcc on this host; checking clang only'
    fi
fi

export compilers

# Compile-only: reads the shards that tools/bootstrap.sh generated for the gates build, writes nothing there.
gen="$root/${GEN_DIR:-build/game/generated}"
extra=()
[ -z "${SACRED_FFMPEG_PREFIX:-}" ] || extra+=("-DSACRED_FFMPEG_PREFIX=$SACRED_FFMPEG_PREFIX")
[ -z "${SACRED_FONT_PREFIX:-}" ] || extra+=("-DSACRED_FONT_PREFIX=$SACRED_FONT_PREFIX")
cmake -S . -B "$out/cmake" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSACRED_BUILD_GAME=ON -DSACRED_GENERATED_DIR="$gen" \
    -DSACRED_ASSETS="${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}" \
    ${extra[@]+"${extra[@]}"} >/dev/null

jobs_file=$out/jobs.txt
python3 -I - "$out/cmake/compile_commands.json" "$root" "$gen" >"$jobs_file" <<'PY'
import json, shlex, sys
entries, root, gen = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3]
for e in entries:
    f = e["file"]
    if not f.startswith((root + "/runtime/", root + "/platform/", gen + "/register.c")):
        continue
    args = shlex.split(e["command"])[1:]
    keep, skip = [], False
    for a in args:
        if skip: skip = False; continue
        if a in ("-o", "-c"):
            skip = a == "-o"; continue
        if a == f or a.startswith("-std=") or a.startswith("-W") or a in ("-g1", "-O1"):
            continue
        keep.append(a)
    lang = "c" if f.endswith(".c") else "c++"
    print(lang + "\t" + f + "\t" + " ".join(shlex.quote(a) for a in keep))
PY

[ -d "$gen" ] || { echo 'generated shards missing' >&2; exit 1; }
listed=$(grep -o '"[^"]*\.c"' "$gen/sources.cmake" | tr -d '"' | sort)
module_shards() { grep "^$gen/$1/" <<<"$listed"; }
largest() { module_shards "$1" | xargs ls -S | sed -n 1p; }
first_with() { module_shards "$2" | xargs grep -l -- "$1" | sed -n 1p; }
shards=$( {
    xargs ls -S <<<"$listed" | sed -n 1p
    for m in sacred granny tincat; do largest "$m"; module_shards "$m" | sed -n 1p; done
    first_with 'x87_' sacred
    first_with 'simd_' sacred
    first_with 'recomp_seh' sacred
    first_with 'recomp_seh' tincat
    first_with 'switch(c->eip)' granny
} | sort -u)
printf 'SHARDS:\n%s\n' "$shards"
shard_flags="-I. -Ithird_party/recomp-kit/runtime -DSACRED_DRIVER_CHECKPOINTS=1"
while IFS= read -r s; do
    printf 'c\t%s\t%s\n' "$s" "$shard_flags"
done <<<"$shards" >>"$jobs_file"

run_job() {
    IFS=$'\t' read -r lang file flags <<<"$1"
    for cc in $compilers; do
        if [ "$lang" = c ]; then
            tool=$cc; std=-std=c11
        else
            case "$cc" in clang) tool=clang++ ;; gcc*) tool=${cc/gcc/g++} ;; *) tool=$cc ;; esac; std=-std=c++17
        fi

        extra=(-Wall -Wextra -Werror -pedantic -O1)
        [ "$cc" = clang ] && extra+=(-Wgnu)
        # shellcheck disable=SC2086
        if ! "$tool" "$std" "${extra[@]}" $flags -c -o /dev/null "$file" 2>"$file.err.$cc.$$"; then
            printf 'PORTABILITY_FAIL %s %s\n' "$cc" "$file"
            cat "$file.err.$cc.$$"
            rm -f "$file.err.$cc.$$"
            return 1
        fi

        rm -f "$file.err.$cc.$$"
    done
}
export -f run_job
count=$(wc -l <"$jobs_file")
# One job line per bash, passed as an argument rather than through -I (BSD xargs caps -I replacements at 255 bytes).
tr '\n' '\0' <"$jobs_file" | xargs -0 -n 1 -P "$(getconf _NPROCESSORS_ONLN)" bash -c 'run_job "$1"' _
printf 'COMPILED %s files with %s\n' "$count" "$compilers"

tools/scan_host_apis.sh
printf 'PORTABILITY_GATE_PASSED\n'
