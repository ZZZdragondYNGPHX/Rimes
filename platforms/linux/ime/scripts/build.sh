#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)

BUILD_DIR="${RIMES_IME_BUILD_DIR:-$IME_ROOT/build}"
PREFIX="${RIMES_IME_PREFIX:-/usr}"
BUILD_TYPE="${RIMES_IME_BUILD_TYPE:-RelWithDebInfo}"

mkdir -p "$BUILD_DIR"
cmake -S "$IME_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DRIMES_BUILD_FCITX5=ON \
    -DRIMES_BUILD_TESTS=ON
cmake --build "$BUILD_DIR" -j"${RIMES_BUILD_JOBS:-$(nproc)}"
echo "Built in $BUILD_DIR (prefix $PREFIX)"
