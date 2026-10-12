# Linux 分发

本分支的主要分发产物是 `fcitx5-rimes` Arch/CachyOS 本地包：

```bash
platforms/linux/ime/scripts/package-arch.sh
sudo pacman -U platforms/linux/ime/dist/fcitx5-rimes-0.2.0-1-x86_64.pkg.tar.zst
```

包仅包含 Linux addon、Buffer/Capsule、审核过的方案和许可证，不包含用户数据。
Android/Windows 发行后续独立处理。本轮不发布 tag、上传 Release 或启动远端构建。
