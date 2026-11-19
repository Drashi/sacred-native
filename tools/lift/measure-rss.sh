#!/usr/bin/env bash
# Run under a bounded monitor alongside build.sh; sample the whole build tree.
set -euo pipefail
root=""
while test -z "$root"; do
    root="$(pgrep -o -f '^cmake --build build/game' || true)"
    if test -z "$root"; then sleep 0.2; fi
done

peak=0
while kill -0 "$root" 2>/dev/null; do
    rss="$(ps -eo pid=,ppid=,rss= | awk -v root="$root" '
        {parent[$1]=$2; memory[$1]=$3}
        END {
            for (pid in parent) {
                ancestor=pid;
                while (ancestor && ancestor!=root) ancestor=parent[ancestor];
                if (ancestor==root) total+=memory[pid];
            }
            print total+0;
        }')"
    if test "$rss" -gt "$peak"; then
        peak="$rss"
        printf 'PEAK_BUILD_RSS_KIB=%s\n' "$peak"
    fi

    sleep 0.2
done

printf 'FINAL_BUILD_RSS_KIB=%s\n' "$peak"
