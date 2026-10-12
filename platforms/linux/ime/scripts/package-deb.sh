#!/usr/bin/env bash
# Build a simple .deb of the Fcitx5 addon + reviewed rime-data.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$IME_ROOT/../../.." && pwd -P)
if [[ ! -f "$REPO_ROOT/scripts/platform-preview/preview.py" ]]; then
    echo "error: could not locate the repository root from $IME_ROOT" >&2
    exit 1
fi
VERSION=$(tr -d '[:space:]' < "$IME_ROOT/VERSION")
ARCH=$(dpkg --print-architecture)
OUT_DIR="${1:-$IME_ROOT/dist}"

WORKDIR=$(mktemp -d /tmp/rimes-deb.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

STAGE="$WORKDIR/data"
python3 "$REPO_ROOT/scripts/platform-preview/preview.py" stage \
    --repo-root "$REPO_ROOT" \
    --output-dir "$STAGE"

BUILD_DIR="$WORKDIR/build"
cmake -S "$IME_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DFCITX_INSTALL_USE_FCITX_SYS_PATHS=ON
cmake --build "$BUILD_DIR" -j"$(nproc)"

ROOT="$WORKDIR/root"
DESTDIR="$ROOT" cmake --install "$BUILD_DIR"
mkdir -p "$ROOT/usr/share/rimes/data"
cp -a "$STAGE"/. "$ROOT/usr/share/rimes/data/"

mkdir -p "$ROOT/DEBIAN"
SIZE=$(du -sk "$ROOT" | awk '{print $1}')
cat > "$ROOT/DEBIAN/control" <<EOF
Package: fcitx5-rimes
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Installed-Size: $SIZE
Depends: fcitx5, librime1t64 | librime1, librime-plugin-lua, libopencc1.1 | libopencc1, libgtk-3-0, libcurl4t64 | libcurl4, libjsoncpp25
Recommends: libgtk-layer-shell0, wl-clipboard
Maintainer: RIMES contributors <https://github.com/scholay/rimes>
Description: RIMES input method frontend for Fcitx 5
 A librime frontend owned by RIMES. It deploys the reviewed RIMES rime-data
 set, keeps user state in ~/.local/share/rimes, and ships the Linux Buffer
 workbench (rimes-buffer) and Capsule rail (rimes-capsule) next to the
 Fcitx5 addon.
EOF

mkdir -p "$OUT_DIR"
DEB="$OUT_DIR/fcitx5-rimes_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$ROOT" "$DEB"
echo "Wrote $DEB"
