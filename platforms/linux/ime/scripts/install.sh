#!/usr/bin/env bash
# Build and install the Fcitx5 RIMES addon plus reviewed rime-data.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$IME_ROOT/../../.." && pwd -P)
if [[ ! -f "$REPO_ROOT/scripts/platform-preview/preview.py" ]]; then
    echo "error: could not locate the repository root from $IME_ROOT" >&2
    exit 1
fi

PREFIX="${RIMES_IME_PREFIX:-/usr}"
BUILD_DIR="${RIMES_IME_BUILD_DIR:-$IME_ROOT/build}"
STAGE_DIR="${RIMES_IME_STAGE_DIR:-}"

usage() {
    cat <<'EOF'
Install the RIMES Fcitx5 addon and its reviewed rime-data tree.

Usage:
  install.sh [--prefix DIR] [--build-dir DIR] [--skip-build]

Default prefix is /usr so the system Fcitx5 discovers the addon. Install after building:
  sudo install.sh --skip-build
EOF
}

SKIP_BUILD=0
while (($# > 0)); do
    case "$1" in
        --prefix)
            PREFIX=$2
            shift 2
            ;;
        --build-dir)
            BUILD_DIR=$2
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if (( SKIP_BUILD == 0 )); then
    RIMES_IME_PREFIX="$PREFIX" RIMES_IME_BUILD_DIR="$BUILD_DIR" \
        "$SCRIPT_DIR/build.sh"
fi

if [[ -z "$STAGE_DIR" ]]; then
    STAGE_DIR=$(mktemp -d /tmp/rimes-ime-stage.XXXXXX)
    trap 'rm -rf "$STAGE_DIR"' EXIT
    python3 "$REPO_ROOT/scripts/platform-preview/preview.py" stage \
        --repo-root "$REPO_ROOT" \
        --output-dir "$STAGE_DIR"
fi

# The prefix was compiled into resource and companion paths; do not silently relocate it.
CONFIGURED_PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
if [[ "$PREFIX" != "$CONFIGURED_PREFIX" ]]; then
    echo "error: prefix differs from the configured build ($CONFIGURED_PREFIX); rebuild with RIMES_IME_PREFIX=$PREFIX" >&2
    exit 1
fi
DESTDIR="${DESTDIR:-}"
export DESTDIR
cmake --install "$BUILD_DIR"

DATA_DEST="${DESTDIR}${PREFIX}/share/rimes/data"
mkdir -p "$DATA_DEST"
cp -a "$STAGE_DIR"/. "$DATA_DEST/"

cat <<EOF
Installed the RIMES Fcitx5 addon, Buffer workbench, and Capsule rail.
  prefix:  $PREFIX
  addon:   Fcitx5 addon directory from the configured CMake build
  buffer:  $PREFIX/libexec/rimes/rimes-buffer and $PREFIX/bin/rimes-buffer
  capsule: $PREFIX/libexec/rimes/rimes-capsule and $PREFIX/bin/rimes-capsule
  ctl:     $PREFIX/bin/rimes-buffer-ctl and $PREFIX/bin/rimes-capsule-ctl
  data:    $PREFIX/share/rimes/data
On KDE Wayland select Fcitx 5 in System Settings > Virtual Keyboard, then log out/in.
Add "RIMES" in fcitx5-configtool. Existing user data is preserved.
Toggle Buffer with Ctrl+Shift+B and Capsule with Ctrl+Shift+V while RIMES is current.
User schema/state lives in \$XDG_DATA_HOME/rimes, isolated from fcitx5-rime.
EOF
