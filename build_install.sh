#!/usr/bin/env bash
# Primary local entrypoint: native Linux IME, preserving all user dictionaries.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
case "${1:-}" in
    --build-only|'') ;;
    -h|--help) echo 'Usage: ./build_install.sh [--build-only]'; exit 0 ;;
    *) echo 'Usage: ./build_install.sh [--build-only]' >&2; exit 2 ;;
esac
"$ROOT/platforms/linux/ime/scripts/build.sh"
BUILD_DIR=${RIMES_IME_BUILD_DIR:-$ROOT/platforms/linux/ime/build}
ctest --test-dir "$BUILD_DIR" --output-on-failure
if [[ "${1:-}" == --build-only ]]; then exit 0; fi
PREFIX=${RIMES_IME_PREFIX:-/usr}
if [[ -w "$PREFIX" || $EUID == 0 ]]; then
    "$ROOT/platforms/linux/ime/scripts/install.sh" --prefix "$PREFIX" --build-dir "$BUILD_DIR" --skip-build
else
    sudo "$ROOT/platforms/linux/ime/scripts/install.sh" --prefix "$PREFIX" --build-dir "$BUILD_DIR" --skip-build
fi
