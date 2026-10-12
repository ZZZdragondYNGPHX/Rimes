# 贡献

主要开发环境是 CachyOS / KDE Plasma / Wayland。先阅读 [ARCHITECTURE.md](ARCHITECTURE.md)。

```bash
platforms/linux/ime/scripts/build.sh
ctest --test-dir platforms/linux/ime/build --output-on-failure
```

每阶段及任务完成时，只在本地执行最小相关验证。输入与焦点变更按需运行隔离的
Fcitx5 testfrontend；真实 KDE/Qt/GTK 宿主验收单独记录。不自动发布、推送或运行远端检查。

投递使用 Fcitx5 `commitString`，每个上下文独立会话；更新保留用户词库与持久笔记。
Android/Windows 为后续目标，不为 Linux 变更执行整个平台构建。

AI 实质参与的提交按 [CONTRIBUTORS.md](CONTRIBUTORS.md) 添加已验证的 `Co-authored-by`。
