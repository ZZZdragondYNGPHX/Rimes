# CachyOS / KDE Plasma 本地迁移验证

环境：CachyOS x86_64，KDE Plasma，Wayland，GCC 16.2.1，Qt 6.12.0，librime 1.17.0，Lua 5.5.1。

## 变更

- 删除 macOS/iOS 原生代码、Swift 主工程、Apple 资源、签名/安装器/发布工作流。
- 默认入口切换到 Linux Fcitx5/C++；支持 Qt6 测试宿主和 Arch/CachyOS 依赖/安装包。
- 依安装前缀查找 Buffer/Capsule companion；默认安装 `/usr` 以匹配系统 Fcitx5 搜索路径。
- 修复 Lua 5.5 禁止修改 numeric-for 循环变量导致辅码脚本无法加载的问题。
- Android 的 24 个非 Apple 引擎资源/许可证/依赖锁迁至 `Shared/Engine`，逐文件确认与原 Git 内容字节一致。
- OfficialPlugins 改为去除 Apple 原生目录/映射的可审查快照，保留来源、许可与哈希锁。

## 最小相关本地验证

- Linux native build（包含 Qt6 host）：通过。
- 14 项 CTest：全部通过。
- 隔离 librime smoke：`nihao + Space → 你好`、数字选择、分页、Escape、部署/重建与目录锁通过。
- 隔离 Fcitx5 testfrontend：普通输入、Buffer/Capsule 投递、焦点失效、密码保护与 companion respawn 通过。
- 插件导入 5 项测试及快照 hash/import 验证：通过，未重新生成 Apple 源码。
- 输入方案数据工具 39 项测试：通过；已移除 Apple importer harness，数据检查不再声称验证原生输入。
- 55 文件审核闭包：通过。
- 根目录 CMake 配置：通过。
- 本机 Buffer/Capsule preview 均确认 `layer: true`。未执行用户真实宿主输入注入；preview 出现既有 GTK 退出/缺少 appmenu 模块消息，面板仍成功显示并退出。
- Arch package 内容/运行路径/Lua 修复：通过。pacman 安装记录 121 个文件、0 缺失。
- Fcitx5 Refresh 后识别 addon，已添加并选中 `rimes`；进程映射确认加载 `/usr/lib/fcitx5/rimes.so`。
- 本机五个核心方案已部署，用户目录生成五个 `.table.bin`。原输入法列表保留，变更前配置备份在 `~/.local/state/rimes/fcitx5-profile-before-rimes`。
- `git diff --check`：通过。未触发远端 CI、发布、上传或提交。

安装包：`platforms/linux/ime/dist/fcitx5-rimes-0.1.0-1-x86_64.pkg.tar.zst`（本地忽略产物）。
SHA-256：`ca7f68abc5738d10d3172192297e731637785385adda1e22a1161fffc11bdf54`。

## 验收边界

尚未验证用户在 Kate/Konsole/Firefox 等真实宿主中的连续打字、混合缩放与多屏拖动。
AI/翻译/音乐/自绘候选/Mailbox/密码库及跨批并击配对未移植；Android/Windows 保留，未构建。
