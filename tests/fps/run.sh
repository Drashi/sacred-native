#!/usr/bin/env bash
# usage: run.sh LABEL SCRIPT_FPS [ENV=VALUE...]   e.g. run.sh v120 120 SACRED_FPS_LIMIT=120 SACRED_QA_FRAME_DT_US=8333
# Needs a SACRED_QA build (build/game, or BIN=...; GAME_MODULE=... for a runtime built with SACRED_GAME_MODULE).
# Results: build/perf/fps/LABEL.log and profile-LABEL/evidence/ppos.csv.
set -u
S=$(cd "$(dirname "$0")" && pwd); label=$1; fps=$2; shift 2
R=$(cd "$S/../.." && pwd)
bin=${BIN:-$R/build/game/sacred-native}
module=(); [ -n "${GAME_MODULE:-}" ] && module=(--game-module "$GAME_MODULE")
assets=${ASSETS:-${SACRED_ASSETS:?set SACRED_ASSETS}}
prof=$R/build/perf/fps/profile-$label; rm -rf "$prof"; mkdir -p "$prof/SAVE"
python3 "$S/${GEN:-gen.py}" "$fps" "$prof/run.script"
# The player's own Settings.cfg, windowed, without sound and movies.
sed -E 's/^(SHOWMOVIE|SOUND|FULLSCREEN) : 1/\1 : 0/' "$assets/Settings.cfg" > "$prof/Settings.cfg"
# Every log line gets a host time stamp; the game is stopped as soon as the script completes or fails.
stamp='$|=1; printf "%.3f %s", time, $_;
    if (/script_complete|RUN_FAILURE/) { system("pkill", "-f", "--", "--profile $ENV{PROF} "); exit }'
timeout ${TIMEOUT:-900} env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe SACRED_QA_PPOS=1 "$@" \
    "$bin" ${module[@]+"${module[@]}"} --assets "$assets" --profile "$prof" \
    --evidence "$prof/evidence" --qa-script "$prof/run.script" 2>&1 |
    PROF="$prof" perl -MTime::HiRes=time -ne "$stamp" > "$R/build/perf/fps/$label.log"
tail -3 "$R/build/perf/fps/$label.log"; wc -l "$prof/evidence/ppos.csv"
