# 本机维护

先关闭临时 Buffer，再更新程序；临时内容可以丢弃，持久数据保留。

```bash
./build_install.sh
```

KDE Wayland 由 KWin 启动 Fcitx5；安装后注销/登录以载入新 addon。
检查输入方案、Qt/GTK 宿主输入、候选与 Buffer/Capsule 焦点行为。
部署仍在运行时，Fcitx5 退出可能等待维护完成；不要启动第二个实例访问同一用户目录。

用户数据位于 `~/.local/share/rimes`（或 `$XDG_DATA_HOME/rimes`）。不要清空该目录来重装。
通过 pacman 安装的版本可用 `sudo pacman -R fcitx5-rimes` 卸载；卸载不移除用户数据。
直接源码安装的路径记在构建目录 `install_manifest.txt`，不得用通配符删除系统目录。
