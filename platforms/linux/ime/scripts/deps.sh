#!/usr/bin/env bash
# Install the native Linux build/runtime dependencies; display tests are optional.
set -euo pipefail
mode=${1:---print}
case "$mode" in --print|--install) ;; *) echo 'Usage: deps.sh [--print|--install]' >&2; exit 2 ;; esac
if command -v pacman >/dev/null; then
    # Current Arch/CachyOS librime builds include Lua; there is no librime-lua package.
    packages=(base-devel cmake ninja extra-cmake-modules pkgconf gettext fcitx5
              fcitx5-qt fcitx5-gtk fcitx5-configtool librime opencc curl jsoncpp gtk3
              qt6-base gtk-layer-shell wl-clipboard)
    manager=(pacman -S --needed)
elif command -v apt-get >/dev/null; then
    packages=(build-essential cmake ninja-build extra-cmake-modules pkg-config gettext
              libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev fcitx5-modules-dev
              fcitx5 fcitx5-modules fcitx5-frontend-gtk3 fcitx5-frontend-qt6
              librime-dev librime-plugin-lua librime-bin libopencc-dev libopencc-data
              libcurl4-openssl-dev libjsoncpp-dev
              libgtk-3-dev libgtk-layer-shell-dev qt6-base-dev wl-clipboard)
    manager=(apt-get install)
else
    echo 'Install CMake, a C++ compiler, Fcitx5 development headers, librime with Lua, OpenCC, GTK3, Qt6 and gtk-layer-shell.' >&2
    exit 1
fi
if [[ "$mode" == --install ]]; then
    if (( EUID == 0 )); then "${manager[@]}" "${packages[@]}";
    else sudo "${manager[@]}" "${packages[@]}"; fi
else
    printf 'sudo'; printf ' %q' "${manager[@]}" "${packages[@]}"; printf '\n'
fi
