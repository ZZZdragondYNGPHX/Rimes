# RIMES for CachyOS / KDE Plasma

This fork targets Linux x86_64 with KDE Plasma on Wayland. It uses the native
Fcitx5/librime addon and GTK3 Buffer and Markdown Capsule companions. macOS/iOS
native targets have been removed. Android and Windows remain future targets.

```bash
platforms/linux/ime/scripts/deps.sh --install
./build_install.sh
```

The default install prefix is `/usr`. Installation requires sudo. Use
`./build_install.sh --build-only` for local compilation and unit checks.
On KDE Wayland select **Fcitx 5** in System Settings > Virtual Keyboard, log
out/in and add **RIMES** in `fcitx5-configtool`. See the
[official Wayland guide](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland#KDE_Plasma).

Current features and remaining gaps are in the [primary README](README.md)
and [Linux IME documentation](platforms/linux/ime/README.md).
User dictionaries stay in `$XDG_DATA_HOME/rimes` (default `~/.local/share/rimes`).
No installation step deletes them.

Based on [RIMES](https://github.com/scholay/rimes) and
[librime](https://github.com/rime/librime). Retain the included license,
copyright, NOTICE and third-party notices when distributing this fork.
