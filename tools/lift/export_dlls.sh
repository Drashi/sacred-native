#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"
export SACRED_ROOT="$PWD" SACRED_ASSETS
headless="${GHIDRA_HOME:?set GHIDRA_HOME to a Ghidra 12.1.4 installation}/support/analyzeHeadless"
mkdir -p ghidra-proj/dlls analysis/export/dll-home analysis/export/dll-tmp
export HOME="$PWD/analysis/export/dll-home"
export XDG_CONFIG_HOME="$HOME/.config"
export _JAVA_OPTIONS="-Xmx3g -Duser.home=$HOME -Djava.io.tmpdir=$PWD/analysis/export/dll-tmp"
for module in granny tincat2; do
    case "$module" in
        granny) hash=9ac73f4aa5cc5f0096bac3bc79b299bcae0e839d08a470fc14b924142c643d68 ;;
        tincat2) hash=f134adde22cc50052dd0abd9fcc1dcea2807f0733dee45f099e1119b644ed137 ;;
    esac

    operation=(-import "$SACRED_ASSETS/$module.dll")
    if test -f "ghidra-proj/dlls/$module.gpr"; then
        operation=(-process "$module.dll" -noanalysis -readOnly)
    fi

    observed=()
    if test "$module" = granny; then
        entries=$PWD/tools/lift/code-pointer-entries-granny.txt
        observed=(-preScript AdmitObservedEntry.java "vtables,immediates,entries-file:$entries" "$hash")
    fi

    if test "$module" = tincat2; then
        entries=$PWD/tools/lift/code-pointer-entries-tincat2.txt
        observed=(-preScript AdmitObservedEntry.java "0x3a770,entries-file:$entries" "$hash")
    fi

    "$headless" "$PWD/ghidra-proj/dlls" "$module" "${operation[@]}" \
        -max-cpu 2 -analysisTimeoutPerFile 600 \
        -scriptPath "$PWD/tools/ghidra_scripts" \
        "${observed[@]}" \
        -postScript ExportRecompileIR.java "$PWD/analysis/export/$module" \
        "$module.dll" "$SACRED_ASSETS/$module.dll" "$hash"
done
