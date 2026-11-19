#!/usr/bin/env bash
# Every gate in one command (the export check runs only with GHIDRA_HOME), against the one build directory that contains
# the whole game and every test target.
# build dir: build/game (SACRED_BUILD_GAME=ON; gcc on Linux, Apple clang on macOS); `ctest --test-dir build/game`
#   registers all tests.
# (build/debug and build/portability are other configurations: a ctest there runs a subset, and the portability tree
# only compiles for the clang/gcc strict-mode check.)
set -euo pipefail
cd "$(dirname "$0")/.."
export SACRED_ASSETS="${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"
dir=build/game
step() { printf '\n=== %s ===\n' "$*"; }
darwin=0; [ "$(uname -s)" = Darwin ] && darwin=1
[ "$darwin" = 0 ] || bash tools/build_macos_deps.sh
bash tools/bootstrap.sh
# Optional prefixes for machines without system -dev packages: SACRED_FFMPEG_PREFIX, SACRED_FONT_PREFIX.
extra=(-DSACRED_STRICT=ON -DSACRED_BUILD_GAME=ON)
[ -z "${SACRED_FFMPEG_PREFIX:-}" ] || extra+=("-DSACRED_FFMPEG_PREFIX=$SACRED_FFMPEG_PREFIX")
[ -z "${SACRED_FONT_PREFIX:-}" ] || extra+=("-DSACRED_FONT_PREFIX=$SACRED_FONT_PREFIX")

if [ "$darwin" = 1 ]; then
    step "clang build: all targets in $dir"
    cmake -S . -B "$dir" "${extra[@]}"
    if ! grep -q 'CMAKE_CXX_COMPILER_ID "AppleClang"' "$dir"/CMakeFiles/*/CMakeCXXCompiler.cmake; then
        echo 'build dir is not an Apple clang build' >&2
        exit 1
    fi
else
    step "gcc build: all targets in $dir"
    if [ -d "$dir" ]; then cmake -S . -B "$dir" "${extra[@]}"
    else cmake -S . -B "$dir" "${extra[@]}" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++; fi
    if ! grep -q 'CMAKE_CXX_COMPILER_ID "GNU"' "$dir"/CMakeFiles/*/CMakeCXXCompiler.cmake; then
        echo 'build dir is not a gcc build' >&2
        exit 1
    fi
fi

grep -q '^SACRED_BUILD_GAME:BOOL=ON' "$dir/CMakeCache.txt" || { echo 'build dir does not build the game' >&2; exit 1; }
cmake --build "$dir" --parallel "${JOBS:-$(getconf _NPROCESSORS_ONLN)}"

step "ctest"
ctest --test-dir "$dir" --output-on-failure | tee "$dir/ctest.log"
# The summary reads "100% tests passed, 0 tests failed out of N" or, in newer ctest, "100% tests passed out of N".
summary=$(grep -E '% tests passed' "$dir/ctest.log" | tail -n 1 || true)
if printf '%s\n' "$summary" | grep -Eq '[1-9][0-9]* tests failed'; then echo "ctest: $summary" >&2; exit 1; fi
total=$(printf '%s\n' "$summary" | sed -n 's/^.* out of \([0-9]*\)$/\1/p')
[ -n "$total" ] && [ "$total" -ge 15 ] || { echo "ctest ran ${total:-0} tests (expected at least 15)" >&2; exit 1; }
echo "CTEST_COUNT $total"

step "check-portability (clang and gcc strict modes + host-API scan)"
bash tools/check_portability.sh
step "tools/check_format.sh"
bash tools/check_format.sh
if [ -n "${GHIDRA_HOME:-}" ]; then
    step "tools/lift/check_export.sh"
    bash tools/lift/check_export.sh
fi

echo GATES_OK
