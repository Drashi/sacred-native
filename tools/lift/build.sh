#!/usr/bin/env bash
# Generated files and compilation products remain private under build/. Linux only (flock, GNU time, du and df).
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/game
exec 9>build/build.lock
flock -n 9 || { printf 'Another build owns this build root.\n' >&2; exit 1; }
bash tools/bootstrap.sh
cmake -S . -B build/game -DSACRED_STRICT=ON -DSACRED_BUILD_GAME=ON
/usr/bin/time -v cmake --build build/game --target sacred-native --parallel 2 "$@"
file build/game/sacred-native
du -sb build/game/generated build/game
df -B1 .
