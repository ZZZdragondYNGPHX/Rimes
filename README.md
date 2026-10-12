# RIMES · CachyOS / KDE Plasma

本分支以这台 CachyOS x86_64、KDE Plasma、Wayland 设备为主要使用环境。
输入法采用 C++ / librime / Fcitx5，Buffer 与 Capsule 使用 GTK3；不再包含 macOS / iOS 原生源码、Apple 工程、签名或安装器。
Android 和 Windows 源码保留为后续目标，不是本轮验收范围。

开发目录为 `/home/henry/Projects/Rimes`；`/home/henry/Software` 用于个人软件，后续开发、提交和推送均在开发目录进行。

## 本机安装

```bash
platforms/linux/ime/scripts/deps.sh --install
./build_install.sh
```

当前 CachyOS 的 `librime` 已包含 Lua 支持，不需要 `librime-lua`。
默认安装到 `/usr`，安装阶段需要 sudo；只编译及验证用 `./build_install.sh --build-only`。
也可以使用 Arch 包管理器安装：

```bash
platforms/linux/ime/scripts/package-arch.sh
sudo pacman -U platforms/linux/ime/dist/fcitx5-rimes-0.3.0-1-x86_64.pkg.tar.zst
```

KDE Wayland：在「系统设置 → 虚拟键盘」选择 **Fcitx 5**，注销并重新登录，
运行 `fcitx5-configtool` 添加 **RIMES**（取消“仅显示当前语言”可查找）。
让 KWin 启动 Fcitx5；环境变量和宿主兼容性遵循 [Fcitx5 官方 Wayland 指引](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland#KDE_Plasma)，不全局强制设置 GTK/Qt 输入模块。

## 当前功能

- 雾凇全拼、自然码双拼、小鹤双拼、五笔 86、英文，以及可选飞耀并击。
- 组字、候选、数字选词、翻页、Escape 取消；每个输入上下文独立 Rime 会话。
- Default Buffer：`Ctrl+Shift+B`，临时分块、Return / 发送按钮上屏。
- Markdown Note Capsule：`Ctrl+Shift+V`，本地笔记、搜索、选择插入。
- Wayland 面板使用 gtk-layer-shell，文本通过 Fcitx5 `commitString` 进入当前输入上下文。
- 本地 AI：异步共享模型、校正 / 续写建议、Tab 或自定义键采纳；保留主动生成、翻译、兼容 API 和原文保护。

打开 Buffer 工具栏「设置」或「RIMES AI 与翻译设置」，默认选择本地轻量模型。模型目录为 `${XDG_DATA_HOME:-$HOME/.local/share}/rimes/models`，CPU 默认 4 线程、1024 token 上下文和 96 token 输出。普通模式只用 Rime，不启动或调用模型。
在目标输入框按 `Ctrl+Shift+B`，点击「AI」或「翻译」异步预加载；AI 对已提交文字给出一条校正 / 续写，句末立即触发，其余停顿默认 250ms。Tab 只采纳完整且有效的建议到 Buffer，发送仍用原操作；翻译按原文单元更新。云端是明确选择的可选渠道，不自动回退。
失焦、组字、原文变化、配置重载及 Password / Sensitive 保护字段淘汰旧请求。使用见 [AI 说明](platforms/linux/buffer/AI.md)，实测见 [本机模型验收](docs/validation/cachyos-local-ai-20261012.md)。

音乐、Mailbox、密码库、媒体 Capsule、跨批并击配对和自绘候选框尚未移植。AI/翻译当前使用内置兼容服务适配，使用已验证的本机 CPU 模型，尚未移植通用下载插件与 CLI 渠道。
本轮完成 Linux 构建与安装适配；真实 KDE 宿主输入的结果单独记录，源码测试不能代替实际打字验证。

## 数据与开发

用户词库：`${XDG_DATA_HOME:-$HOME/.local/share}/rimes`；日志：`${XDG_STATE_HOME:-$HOME/.local/state}/rimes/log`。
更新仅替换安装程序及共享方案，不清空学习词库、配置或 Capsule 笔记，也不覆盖其他 Rime 前端的数据。

```bash
platforms/linux/ime/scripts/build.sh
ctest --test-dir platforms/linux/ime/build -R '^(RimesAI|RimesBufferModel|RimesBufferProtocol)$' --output-on-failure
# 涉及输入/投递时才运行隔离环境的本地验证：
platforms/linux/ime/tests/e2e/e2e.sh --skip-display --skip-wayland
```

根目录也支持 `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr`。
架构见 [ARCHITECTURE.md](ARCHITECTURE.md)，详细输入法说明见 [Linux IME](platforms/linux/ime/README.md)，后续优先级见 [PLATFORM-ROADMAP.md](PLATFORM-ROADMAP.md)。
旧功能文档和 CHANGELOG 记录上游历史，不能视为本分支 Linux 功能承诺。

本版本基于 [RIMES](https://github.com/scholay/rimes) 修改，中文引擎为 [librime](https://github.com/rime/librime)。
主要修改为移除 Apple 原生目标、以 CachyOS / KDE 为默认构建与安装目标。
保留原版权、[LICENSE](LICENSE)、[NOTICE](NOTICE) 与 [第三方声明](THIRD_PARTY_NOTICES.md)。
