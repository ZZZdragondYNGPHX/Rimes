# Linux 0.3.0 · 本地 AI 建议与翻译

主要目标是 i7-11800H / 16GB 的 CachyOS + KDE Plasma / Wayland。本机已经下载模型并完成 CPU 验证；没有连接云端推理服务，也没有替换当前运行中的输入法。

## 使用

安装本地 `fcitx5-rimes-0.3.0-1-x86_64.pkg.tar.zst` 后，在下次登录加载新 addon。此次任务不会自动安装或重启 Fcitx5。
模型文件已放在 `${XDG_DATA_HOME:-$HOME/.local/share}/rimes/models`。另有独立的本地模型归档，包含来源清单、SHA-256 和许可证；模型不会随输入法启动自动下载。

打开 Buffer 的「设置」或 `rimes-buffer --settings`，选择渠道：

- **本地轻量模型**：默认渠道。GGUF 和 llama-server 填绝对路径，默认指向上述模型目录。不需要 API 密钥。
- **已有 localhost API**：填写完整 Chat Completions 地址和模型名称，仅允许 loopback 主机；这个渠道不会启动或管理你的其他服务。
- **云端 API**：必须明确选择，填写 HTTPS 地址、模型与密钥。失败时不切换渠道，也不自动回退云端。本轮未进行云端请求验证。

回到目标输入框，按 `Ctrl+Shift+B` 打开并连接 Buffer。

| 模式 | 行为 |
| --- | --- |
| 默认 | 只使用 Rime；不启动、预加载或调用模型。Buffer 的原文按原有分块方式发送。 |
| AI | 异步预加载本机模型，只针对已提交的 Buffer 文字提供一条建议，明确标为「校正」或「续写」。句末立即触发，其他输入停顿默认 250ms 后触发。 |
| 翻译 | 使用同一模型，继续按原文句子单元更新译文，不显示预测续写建议。句末立即触发，其余默认停顿 250ms。 |

AI 建议独立于 Rime 候选显示，数字仍用于选词。Rime 组字期间不请求建议，输入不等待模型。
「生成中」内容只是流式预览，未完成、未校验或过期的内容不能采纳、复制或发送。保持原文不变的模型输出不会显示为建议。

**Tab 采纳**：仅在 AI 模式、无 preedit、建议完整且修订匹配、捕获目标有效并且不在 Password / Sensitive 保护字段时接管。其他情况透传给原输入流程。一次按下只采纳一次，对应自动重复与 key-up 不会重复采纳。校正只替换冻结的文字范围，续写插入冻结的 Buffer 位置；不会直接上屏。

设置里的「采纳快捷键」使用 Fcitx 键名，例如 `Control+semicolon`。留空关闭，「恢复默认 Tab」重置。会检查 Rime/Buffer/Fcitx 保留键、已配置的 Fcitx 热键和可识别的 KDE 全局快捷键；冲突时提示并保留旧文件。无法识别任意应用自身的所有快捷键。

**主动生成**仍由「生成」按钮启动，结果独立预览，只有完整结果才允许复制或发送。普通 AI 建议的采纳后，发送按钮 / Return / 长按 Return 继续发送 Buffer 原文；主动生成和翻译则发送完整的对应结果。所有上屏仍使用当前 Fcitx5 `commitString`，不模拟粘贴。

## 生命周期、预算与配置

AI 和翻译共用一个 llama-server 子进程。进入高级模式才创建控制线程并异步加载，退出后保留短暂缓存，默认闲置 60 秒终止子进程并释放模型；加载途中退出会取消加载。子进程绑定 `127.0.0.1` 随机端口、使用每次启动独立的私有认证文件，并在所属 Fcitx 退出时终止。

默认 CPU 4 线程、上下文 1024 token、最多输出 96 token、关闭思考模式、单并行槽；不使用 CUDA / PyTorch，也不创建 Python 推理环境。

配置路径 `${XDG_CONFIG_HOME:-$HOME/.config}/rimes/ai.json`，目录 0700、文件 0600，版本 2。界面保存会通知 addon 取消旧请求；手工编辑 JSON 后应重新打开设置并保存，或重新选择模式加载配置。旧版已明确设置的 API 配置仍可读取。

| 字段 | 默认 | 允许范围 / 用途 |
| --- | --- | --- |
| `channel` | `managed_local` | `managed_local` / `local_api` / `cloud_api` |
| `model_file`, `runtime_file` | 用户模型目录中的 GGUF / llama-server | 绝对路径 |
| `idle_seconds` | 60 | 1–3600 秒；设置可调整 |
| `threads` | 4 | 1–4；设置可调整 |
| `debounce_ms` | 250 | 0–2000ms；设置可调整 |
| `context_size` | 1024 | 512–2048 token；JSON 中调整 |
| `max_tokens` | 96 | 16–256 token；JSON 中调整 |
| `accept_key` | `Tab` | 空字符串关闭；设置可调整 |
| `timeout_seconds` | 60 | 5–180 秒 |

建议只取冻结插入位置前最多 64 个 Unicode 字符；校正拒绝过大的改写，续写最多 24 个字符。翻译 / 主动生成仍限制单次输入和输出各 32KiB，总待发送结果最多 256KiB。长文本可能触及上下文或输出预算并报错，应分段处理或显式调整预算；不会将截断结果当作完整建议。

每个请求绑定工作区 epoch、原文修订、块身份、范围、插入位置、配置快照和目标身份。编辑、切模式、配置重载、失焦、保护字段及 preedit 会取消或淘汰旧请求。工作线程只保留最新待处理请求；原文在失败时保留。建议只有显式采纳才改 Buffer，主动生成 / 译文只在对应结果成功上屏后消费原文。

## 本地验证

```bash
platforms/linux/ime/scripts/build.sh
ctest --test-dir platforms/linux/ime/build -R '^(RimesAI|RimesBufferModel|RimesBufferProtocol)$' --output-on-failure
python3 platforms/linux/ime/tests/e2e/ai_local_test.py platforms/linux/ime/build /tmp/rimes-ai-reviewed-data
RIMES_MODEL_HOST="$PWD/platforms/linux/ime/build/rimes-model-host" platforms/linux/ime/build/rimes-ai-real-benchmark 4
platforms/linux/ime/build/rimes-ai-real-fcitx "$PWD/platforms/linux/ime/build" /tmp/rimes-ai-reviewed-data /tmp/rimes-real-fcitx.json
```

测试数据目录可用 `python3 scripts/platform-preview/preview.py stage --repo-root . --output-dir /tmp/rimes-ai-reviewed-data` 生成。
HTTP fixture 只验证状态、错误、SSE、取消、快捷键和投递边界，其时间不用于模型测速。真实测速使用已固定版本的 Qwen3 0.6B Q4_K_M 与 llama.cpp CPU 程序。

真实结果、冷启动 / 预热区分、资源体积、峰值内存和限制见源码中的 `docs/validation/cachyos-local-ai-20261012.md`。少量合成短句不能代表长文本质量或全天使用的可靠性。中文无错短句有时不会提供续写；未进行大规模质量或长时间压力验收。
