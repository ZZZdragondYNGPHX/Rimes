---
name: rimes-project
description: RIMES Linux/Fcitx5 architecture, CachyOS KDE Plasma build, tests and local installation.
---

# RIMES Linux Project

Use `/home/henry/Projects/Rimes` for development and Git operations; `/home/henry/Software` is the personal software directory.

Read `ARCHITECTURE.md` and the relevant `platforms/linux` sources before edits.
CachyOS x86_64, KDE Plasma and Wayland are the primary target. Android and
Windows are retained for later work. Apple native targets are retired.

- Use one Rime session per Fcitx5 InputContext and preserve preedit.
- Commit only through the current Fcitx5 context's `commitString`; do not use
  clipboard, paste, Accessibility or simulated key injection for normal delivery.
- Keep Buffer/Capsule focus identity checks and password-field protection.
- Preserve `$XDG_DATA_HOME/rimes` user dictionaries, settings and persistent notes.
- Run only the minimum relevant local validation at each stage and completion.
- Build with `platforms/linux/ime/scripts/build.sh`. Use relevant CTest targets;
  input changes may require `tests/e2e/e2e.sh --skip-display --skip-wayland` in
  an isolated temporary user directory. Never run Android/Windows builds just
  for Linux changes or invoke remote CI as local verification.
- `./build_install.sh` builds and installs to `/usr`; installation requires sudo.
  `--build-only` skips install. Arch packages use `scripts/package-arch.sh`.
- On KDE Wayland let KWin start Fcitx5 via System Settings > Virtual Keyboard.
  Keep host typing, selected input method and installation checks distinct.
- Finish with `git diff --check`; do not commit generated build/package artifacts.
