#!/usr/bin/env bash
# Adds Sacred Native to the desktop's application menu for the current user (no root, nothing outside your home):
# ${XDG_DATA_HOME:-~/.local/share}/applications/sacred-native.desktop, which starts this bundle's sacred-launcher,
# and the logo in icons/hicolor/{256x256,512x512}/apps/sacred-native.png there.
#   ./install-desktop-entry.sh               install, or update after the bundle moved or was replaced
#   ./install-desktop-entry.sh --uninstall   remove both again
# The entry holds the bundle's absolute path: run it again after moving the bundle.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
entry="$data/applications/sacred-native.desktop"
sizes=(256 512)

refresh() { # the menu and icon caches, where the desktop keeps them (all optional)
    command -v update-desktop-database >/dev/null && update-desktop-database -q "$data/applications" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" 2>/dev/null || true
    command -v xdg-desktop-menu >/dev/null && xdg-desktop-menu forceupdate 2>/dev/null || true
}

case "${1:-}" in
    --uninstall)
        rm -f "$entry"
        for size in "${sizes[@]}"; do rm -f "$data/icons/hicolor/${size}x$size/apps/sacred-native.png"; done
        refresh
        echo "Removed $entry and its icons."
        exit 0
        ;;
    "") ;;
    *) echo "usage: $0 [--uninstall]" >&2; exit 2 ;;
esac

[ -x "$here/sacred-launcher" ] || { echo "$0: no sacred-launcher next to this script" >&2; exit 1; }
case "$here" in *$'\n'*) echo "$0: the bundle's path holds a line break" >&2; exit 1 ;; esac
# Exec: the path in double quotes, with ", `, $ and \ escaped inside them, then every \ doubled again (the desktop
# entry's string escaping) and % written as %% (field codes).
exec_path="$here/sacred-launcher"
exec_path="${exec_path//\\/\\\\}" exec_path="${exec_path//\"/\\\"}"
exec_path="${exec_path//\`/\\\`}" exec_path="${exec_path//\$/\\\$}"
exec_path="${exec_path//\\/\\\\}" exec_path="${exec_path//%/%%}"

mkdir -p "$data/applications"
for size in "${sizes[@]}"; do
    mkdir -p "$data/icons/hicolor/${size}x$size/apps"
    cp "$here/icons/sacred-native-$size.png" "$data/icons/hicolor/${size}x$size/apps/sacred-native.png"
    chmod 644 "$data/icons/hicolor/${size}x$size/apps/sacred-native.png"
done
tmp="$(mktemp "$entry.XXXXXX")"
while IFS= read -r line; do
    [ "$line" = "Exec=@LAUNCHER@" ] && line="Exec=\"$exec_path\""
    printf '%s\n' "$line"
done < "$here/desktop/sacred-native.desktop" > "$tmp"
chmod 644 "$tmp" && mv "$tmp" "$entry"
command -v desktop-file-validate >/dev/null && { desktop-file-validate "$entry" || true; }
refresh
echo "Installed $entry (starts $here/sacred-launcher)."
