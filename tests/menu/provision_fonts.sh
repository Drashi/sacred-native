#!/usr/bin/env bash
# Linux-only development helper; normal system/Brew pkg-config also works.
set -euo pipefail
cd "$(dirname "$0")/../.."
root="$PWD/build/font-deps"
mkdir -p "$root/packages"
cd "$root/packages"
apt-get download libfreetype-dev libfreetype6 \
    libbrotli-dev libpng-dev zlib1g-dev
for package in *.deb; do dpkg-deb -x "$package" "$root"; done
sha256sum ./*.deb
