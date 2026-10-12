# CachyOS AI 与实时翻译本地验证

用户优先级：AI 生成与实时翻译；本轮只完成移植和本地验证，不连接真实 AI 服务。

## 已实现

- Linux 0.2.0 内置 Default / AI / 翻译模式，GTK 服务设置与独立结果栏。
- 云端 HTTPS 或本机兼容 Chat Completions 服务，模型、私有 API 密钥、AI 指令、目标语言与超时设置。
- 独立可取消网络线程，停顿 700 ms 后翻译；stable source block IDs 保留已完成的句子前段。
- 结果必须手动发送；成功上屏后只退休对应原文；复制完整结果不投递或消费。
- workspace、source IDs、请求 epoch、当前捕获上下文与保护态拒绝迟到结果；旧 UI 按钮身份不得发送更新后的结果。
- 私有配置目录 0700 / 文件 0600，拒绝 symlink、非当前用户/公开可读文件和无效字段；密钥不进入快照或日志。

## 最小相关本地检查

- Linux 构建（GCC 16.2.1 / Qt6 / GTK3 / libcurl / JsonCpp）：通过。
- `RimesAI` 与 `RimesBufferProtocol` 两项相关 CTest：通过。
- loopback HTTP fixture：生成/翻译消息、认证头、普通/数组内容、认证失败、无效 JSON/结构、结果大小上限与请求取消通过。只使用 synthetic source 和 fixture key。
- 隔离 Fcitx5 AI integration：生成不自动上屏、原文按结果精准消费、翻译前段保留、过期按钮拒绝、失焦取消及密码保护通过。
- 原有 Default Buffer Fcitx5 regression：通过，包括输入暂存、Return / hold、焦点/拖动与未发送原文保留。
- 本机 KDE Wayland Buffer / 设置窗口预览：通过。最终 AI 面板 geometry 为 1760 × 118，size request 760 × 118，`layer: true`；人工查看截图确认模式选中、结果文字对比度及操作按钮。
- Arch package 编译与文件/依赖检查：通过，含 `rimes-ai-settings.desktop`，不含用户 `ai.json`。
- `git diff --check`、改动脚本语法：通过。未运行远端 CI、发布或提交。

产物：`platforms/linux/ime/dist/fcitx5-rimes-0.2.0-1-x86_64.pkg.tar.zst`（忽略的本地文件）。
SHA-256：`3b65e75a8b9346d1b2ec1c3eda27804e89ed3ef3f7d8304575deff1b1517a333`。

## 状态和边界

系统当前仍安装 0.1.0-1，本轮未执行系统升级或重启输入法。
未验证真实服务、用户 API 密钥、真实宿主中的网络请求/连续打字。
实时翻译按停顿/句子单元持续处理；当前请求使用完整 JSON 响应，不提供 SSE token streaming。
CLI AI 渠道、离线模型、通用下载插件、Mailbox、音乐、密码库、自绘候选框与跨批并击仍为后续移植范围。
