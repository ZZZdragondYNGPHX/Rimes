# 当前系统架构

本分支采用 Linux / Fcitx5 架构，见 [ARCHITECTURE.md](ARCHITECTURE.md)。
入口为 `build_install.sh`，主代码在 `platforms/linux`。
macOS / iOS 的宿主、工程与发布流程已移除；Android / Windows 保留独立目标。
