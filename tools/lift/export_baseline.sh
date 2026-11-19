#!/usr/bin/env bash
# Whole-program IR export of the analysed, otherwise untouched Sacred.exe into analysis/export (input of the
# control_fixtures test).
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"
ghidra_home="${GHIDRA_HOME:?set GHIDRA_HOME to a Ghidra 12.1.4 installation}"
export SACRED_ROOT="$PWD" SACRED_ASSETS
mkdir -p build/export analysis/export/baseline-home analysis/export/baseline-tmp
export HOME="$PWD/analysis/export/baseline-home"
export XDG_CONFIG_HOME="$HOME/.config"
export _JAVA_OPTIONS="-Xmx3g -Duser.home=$HOME -Djava.io.tmpdir=$PWD/analysis/export/baseline-tmp"
"$ghidra_home"/support/analyzeHeadless "$PWD/ghidra-proj" sacred -process Sacred.exe -noanalysis -readOnly \
    -max-cpu 2 -scriptPath "$PWD/tools/ghidra_scripts" \
    -postScript ExportRecompileIR.java "$PWD/analysis/export" 2>&1 | tee build/export/baseline-run.log
if grep -q 'SCRIPT ERROR' build/export/baseline-run.log; then echo "EXPORT_SCRIPT_FAILED" >&2; exit 1; fi
