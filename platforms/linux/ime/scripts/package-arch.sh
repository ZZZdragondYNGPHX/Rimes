#!/usr/bin/env bash
# Package only Linux runtime sources and reviewed data; no user data or remote builds.
set -euo pipefail
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$IME_ROOT/../../.." && pwd -P)
OUT_DIR=${1:-$IME_ROOT/dist}
if (( EUID == 0 )); then echo 'Run package-arch.sh as your regular user, not root.' >&2; exit 1; fi
command -v makepkg >/dev/null || { echo 'Install base-devel first.' >&2; exit 1; }
mkdir -p "$OUT_DIR"
OUT_DIR=$(CDPATH= cd -- "$OUT_DIR" && pwd -P)
WORK_DIR=$(mktemp -d /tmp/rimes-arch.XXXXXX)
trap 'rm -rf "$WORK_DIR"' EXIT
cp "$IME_ROOT/packaging/arch/PKGBUILD" "$WORK_DIR/PKGBUILD"
VERSION=$(tr -d '[:space:]' < "$IME_ROOT/VERSION")
sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$WORK_DIR/PKGBUILD"
tar -czf "$WORK_DIR/rimes-source.tar.gz" \
    --transform='s,^,rimes/,' --exclude='build' --exclude='dist' --exclude='__pycache__' \
    -C "$REPO_ROOT" platforms/linux scripts/platform-preview rime-data \
    LICENSE LICENSES NOTICE THIRD_PARTY_NOTICES.md README.md
# Pin the exact local archive bytes before makepkg verifies and compiles them.
HASH=$(sha256sum "$WORK_DIR/rimes-source.tar.gz" | cut -d ' ' -f1)
sed -i "s/sha256sums=('SKIP')/sha256sums=('$HASH')/" "$WORK_DIR/PKGBUILD"
(cd "$WORK_DIR" && PKGDEST="$OUT_DIR" makepkg --noconfirm --force)
PACKAGE_PATH=$(cd "$WORK_DIR" && PKGDEST="$OUT_DIR" makepkg --packagelist)
printf 'Local package written to %s\nInstall with: sudo pacman -U %q\n' "$OUT_DIR" "$PACKAGE_PATH"
