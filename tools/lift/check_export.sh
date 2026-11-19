#!/usr/bin/env bash
# Gate: regenerate the Sacred.exe export from the committed recipe into a scratch directory and compare it with the
# hashes recorded in tools/lift/export-sacred.sha256. Also verifies the export that the shards were built from.
# Headless Ghidra only; needs no display. The scratch directory stays under analysis/export (exporter rule).
set -euo pipefail
# macOS has shasum, not GNU coreutils sha256sum (same output and --check format).
command -v sha256sum >/dev/null || sha256sum() { shasum -a 256 "$@"; }
cd "$(dirname "$0")/../.."
recorded=tools/lift/export-sacred.sha256
trial=analysis/export/gate-check
rm -rf "$trial"; mkdir -p "$trial"
EXPORT_OUT="$PWD/$trial" bash tools/lift/export_exe.sh > "$trial.log" 2>&1 || { tail -20 "$trial.log"; exit 1; }
measure() {
    printf 'blocks_ir_sha256=%s\n' "$(sha256sum "$1/blocks.jsonl.gz" | cut -d' ' -f1)"
    printf 'ir_sha256=%s\n' "$(sha256sum "$1/instructions.jsonl.gz" | cut -d' ' -f1)"
    printf 'instructions=%s\n' "$(gzip -dc "$1/instructions.jsonl.gz" | wc -l | tr -d " ")"
}
measure "$trial" > "$trial/measured.txt"
if ! diff -u "$recorded" "$trial/measured.txt"; then echo "EXPORT_NOT_REPRODUCED"; exit 1; fi
assets="${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"
if ! python3 -I tools/lift/sweep_code_pointers.py --exe "$assets/Sacred.exe" --export "$trial" \
    --check tools/lift/code-pointer-entries-sacred.txt --must 0x70ec30; then
    echo "CODE_POINTER_ENTRIES_FAILED"
    exit 1
fi
if test -d analysis/export/sacred-startup; then
    measure analysis/export/sacred-startup > "$trial/shipped.txt"
    diff -u "$recorded" "$trial/shipped.txt" || { echo "EXPORT_SHIPPED_STALE"; exit 1; }
fi

echo "EXPORT_REPRODUCED $(tr '\n' ' ' < "$recorded")"
