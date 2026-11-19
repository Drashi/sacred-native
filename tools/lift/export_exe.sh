#!/usr/bin/env bash
# Recover observed EXE entries only in a private copy; preserve the source project.
set -euo pipefail
# macOS has shasum, not GNU coreutils sha256sum (same output and --check format).
if command -v sha256sum >/dev/null; then sha=(sha256sum); else sha=(shasum -a 256); fi
cd "$(dirname "$0")/../.."
: "${SACRED_ASSETS:?set SACRED_ASSETS to your Sacred Gold installation}"
ghidra_home="${GHIDRA_HOME:?set GHIDRA_HOME to a Ghidra 12.1.4 installation}"
export SACRED_ROOT="$PWD" SACRED_ASSETS
mkdir -p build/export analysis/export/startup-home analysis/export/startup-tmp
find ghidra-proj/sacred.rep -type f -print0 |
    sort -z | xargs -0 "${sha[@]}" > build/export/preserved-project.sha256
if ! test -f ghidra-proj/startup-recovery.gpr; then
    cp -a ghidra-proj/sacred.rep ghidra-proj/startup-recovery.rep
    cp -a ghidra-proj/sacred.gpr ghidra-proj/startup-recovery.gpr
fi

export HOME="$PWD/analysis/export/startup-home"
export XDG_CONFIG_HOME="$HOME/.config"
export _JAVA_OPTIONS="-Xmx3g -Duser.home=$HOME -Djava.io.tmpdir=$PWD/analysis/export/startup-tmp"
hash=db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74
# The entries AdmitObservedEntry.java admits before the export (see the script for the forms).
admit=(
    repair:0x34656f:0x346575 repair:0x4000:0x4006 repair:0x10040:0x10043 repair:0xd525c:0xd5279
    repair:0xe4947:0xe494d repair:0x135448:0x13544e repair:0x144e43:0x144e68 repair:0x145344:0x145363
    repair:0x14583e:0x145842 repair:0x1521ed:0x1521ff repair:0x217373:0x2173c3 repair:0x266665:0x26666c
    repair:0x266922:0x266926 repair:0x266e67:0x266e6e repair:0x2e005b:0x2e0064 repair:0x330023:0x330029
    repair:0x360059:0x36006e repair:0x37005b:0x37005f repair:0x3cfffb:0x3d0005 repair:0x401ffc:0x402008
    repair:0x40ff7d:0x40ff82 table:0x4e7000:0x4e71c0 table:0x4e71c4:0x4e71d8 table:0xf46f0:0xf474c
    table:0xf83f0:0xf83fc table:0x136c6c:0x136cf0 table:0x136c94:0x136cf0 table:0x136ccc:0x136cf0
    table:0x136de4:0x136e4c table:0x19f390:0x19f42c table:0x1c8cb0:0x1c8cc8 table:0x313660:0x313670
    table:0x136ebc:0x136ef0 table:0x136f48:0x136f68 table:0x13703c:0x137060 vtables immediates
    entries-file:$PWD/tools/lift/code-pointer-entries-sacred.txt
)
admit_spec=$(IFS=,; echo "${admit[*]}")
"$ghidra_home"/support/analyzeHeadless \
    "$PWD/ghidra-proj" startup-recovery -process Sacred.exe -noanalysis -readOnly \
    -max-cpu 2 -scriptPath "$PWD/tools/ghidra_scripts" \
    -preScript AdmitObservedEntry.java "$admit_spec" "$hash" \
    -postScript ExportRecompileIR.java "${EXPORT_OUT:-$PWD/analysis/export/sacred-startup}" 2>&1 |
    tee build/export/exe-run.log
# analyzeHeadless exits 0 when a script throws; a half-run recipe must never produce an export.
if grep -q 'SCRIPT ERROR' build/export/exe-run.log; then echo "EXPORT_SCRIPT_FAILED" >&2; exit 1; fi
"${sha[@]}" --check build/export/preserved-project.sha256
printf 'PRESERVED_SACRED_PROJECT_UNCHANGED\n'
