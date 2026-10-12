# CachyOS 本机轻量模型与 Linux 0.3.0 验收

日期：2026-10-12。目标：Intel i7-11800H（8 核 / 16 线程）、16GB 内存，CachyOS x86_64 / KDE Plasma / Wayland。CPU 推理，无 CUDA / PyTorch 或额外 Python 推理环境。

开发仓库已从 `/home/henry/Software/Rimes` 整体迁移到 `/home/henry/Projects/Rimes`，保留 `.git`、已有迁移改动、本地构建与安装包。`Software` 是个人软件目录；后续开发、提交与远端推送均在 `Projects` 进行。用户数据、模型和当前运行输入法未替换。

## 实现与边界

- Default 只用 Rime：不创建模型控制线程、启动模型或推理。
- AI / 翻译在独立 llama-server 进程异步加载，共用一个实例；默认退出后闲置 60 秒卸载，可配置。退出加载中的模式直接取消加载。
- 模型通过带私有认证的随机 localhost 端口接入，保留已有兼容 API。云端是明确选择的独立渠道，无自动回退。
- AI 仅分析已提交 Buffer，组字期间不请求。句末立即触发，其他输入默认 250ms 停顿。建议定时器使用 1ms 精度避免时间事件合并增加等待。
- 流式预览标为生成中，不可采纳 / 复制 / 发送；完成后从冻结原文推导校正范围或续写位置，拒绝大的改写。每次一条，不自动改字，不占 Rime 数字候选操作。
- Tab / 自定义 / 关闭 / 恢复默认均已实现。配置冲突拒绝保存，保留旧配置；按键 latch 保证一次采纳并消费对应 repeat / key-up。
- 请求绑定原文、块身份、范围、插入位置、目标、配置快照与 epoch。原文改变、切模式、配置重载、失焦、Password / Sensitive 或 preedit 使旧请求失效。
- 校正 / 续写只改 Buffer 的冻结范围；发送沿用 `commitString`。模型或网络失败不消费原文。

## 模型、来源和程序体积

模型：[Unsloth Qwen3-0.6B-GGUF](https://huggingface.co/unsloth/Qwen3-0.6B-GGUF)，Q4_K_M，固定 revision `50968a4468ef4233ed78cd7c3de230dd1d61a56b`。源权重为 [Qwen/Qwen3-0.6B](https://huggingface.co/Qwen/Qwen3-0.6B)，Apache-2.0；量化文件的许可标注一致，已附完整许可证。

运行时：[llama.cpp 固定源码](https://github.com/ggml-org/llama.cpp/tree/d81235049384534c167caea52b85a694f6103d14)，ref `v0.6.0`，实际 commit `d81235049384534c167caea52b85a694f6103d14`，程序报告 `0.6.0-dev (build 1, commit d812350)`，MIT。本机 GCC 编译 CPU 静态后端，`GGML_NATIVE=ON`，CUDA / Vulkan / 动态后端关闭；不能据此承诺其他 CPU 的二进制兼容性。

| 文件 | 实测字节 |
| --- | ---: |
| Qwen3-0.6B-Q4_K_M.gguf | 396,705,472 |
| llama-server | 20,038,456 |
| 模型、runtime 和两份许可证 | 416,756,349（约 397.45MiB） |
| 输入法 Arch 包 | 20,061,457（压缩约 19.13MiB；安装约 62.98MiB） |
| 独立模型归档 | 395,299,317（压缩约 376.99MiB） |

加上输入法和必要 host 程序，仍低于 1GB 文件预算。已存在的系统 Fcitx / GTK / librime 依赖不重复计入模型下载。完整 manifest、编译参数、许可证和 SHA-256 见 [model-manifest.json](cachyos-local-ai-20261012/model-manifest.json)。

GGUF SHA-256：`ac2d97712095a558e31573f62f466a3f9d93990898b0ec79d7c974c1780d524a`。

## 真实 CPU 测量

均调用实际 GGUF，不以 fixture 的延迟替代模型测速。配置：1024 token 上下文、96 token 输出、单并行槽、关闭思考，先 2 线程，再比较 4 线程并将默认设为 4。

“冷启动”指新建服务进程到 `/health` 就绪，未清除操作系统页缓存。server 自带权重 warmup 后的首次聊天仍需提示词预填充；“预热”表中的请求在此前真实推理之后执行，并复用同一进程 / 提示词前缀。样本很少，无统计可靠性或普遍延迟保证。

首个可见结果按**与原文不同的、可显示的预览文字**计时；JSON 前缀、网络首 token、加载提示和仅重复原文的内容均不算。`-1` 表示没有新建议，不能计为低延迟成功。完整时间是响应完成及本地建议校验，不含用户按键。

| 项目 | 2 线程 | 4 线程 |
| --- | ---: | ---: |
| 进程冷启动就绪 | 759ms | 760ms |
| 首次聊天完成（无新建议） | 926ms | 548ms |
| 预热校正：网络首 content delta | 127ms | 84ms |
| 预热校正：首个可见预览 | 271ms | 191ms |
| 预热校正：完整有效建议 | 351ms | 253ms |
| 预热续写：网络首 content delta | 94ms | 64ms |
| 预热续写：首个可见预览 | 338ms | 249ms |
| 预热续写：完整有效建议 | 459ms | 334ms |
| 服务峰值 RSS（VmHWM） | 821,480KiB（约 802MiB） | 821,888KiB（约 803MiB） |
| 测试配置闲置 1 秒后的卸载 | 1,057ms | 1,057ms |

峰值内存来自 `/proc/<llama-server>/status` 的 VmHWM，10ms 轮询；host 被 exec 替换为 server，计入同一服务进程。低于 1–1.5GB 目标。记录机器仍有桌面及其他程序运行，未做专用机器的系统调度控制。

样本结果：

- `今天天气气很好。` → `今天天气很好。`，有效校正，只删除冻结范围内的一个重复 `气`。
- `I am going to the` → 追加 ` school.`，有效续写。停顿 250ms 单独计入，表中推理延迟从请求触发后算。
- `我今天很开心。`、`我明天去学校。` 返回原文，无新建议；没有自动改写或把原文当作续写。
- 翻译 `我明天去学校。` → `I will go to school tomorrow.`，同一模型实例，未混入预测续写。

原始数据：[2 线程](cachyos-local-ai-20261012/cpu-2threads.json)、[4 线程](cachyos-local-ai-20261012/cpu-4threads.json)。早期未约束格式的输出被拒绝并保留原文；这些失败输出不计入最终成功建议的测速。

## 真实模型贯穿隔离 Fcitx

`rimes-ai-real-fcitx` 使用真正的 Fcitx addon / Rime / Buffer / model-host / GGUF，只创建合成测试输入上下文，不连接或替换正在运行的输入法。

| 项目 | 实测 |
| --- | ---: |
| Default 下模型子进程 | 0 |
| 异步加载期间 Rime 按键返回 | 14.02ms |
| 从进入 AI 到就绪状态快照（含状态轮询） | 914.94ms |
| 第一次校正预览（提示词尚未预填充） | 456.28ms |
| 第一次完整有效校正 | 532.74ms |
| 连续输入的实际停顿触发等待 | 256.27ms |
| 预热续写首次可见快照，触发后 | 268.35ms |
| 预热续写完整有效快照，触发后 | 335.44ms |
| 退出到模型进程消失（配置 1 秒） | 1,059.74ms |

该路径确认：源文字在采纳前不变，Tab 只改 Buffer，之后真实 `commitString` 投递到冻结上下文；后续续写共用同一个模型进程；退出先保留缓存再卸载。

快照测试以 10ms 间隔观察发给 renderer 的数据，触发点按首次 busy 快照估计，具有约 10ms 采样误差；不把它称作屏幕逐帧像素测速。原始结果：[fcitx-real-model.json](cachyos-local-ai-20261012/fcitx-real-model.json)。Wayland GTK 预览确认为 layer-shell，建议行加入后的高度 158px，见 [geometry](cachyos-local-ai-20261012/wayland-suggestion-geometry.json)。

## 最小相关本地验证

- `RimesAI`、`RimesBufferModel`、`RimesBufferProtocol`：通过。覆盖私有配置、校正范围、续写位置、原文保留、修订 / 光标过期、保守改写拒绝、快捷键冲突及旧配置保留。
- localhost fixture + 隔离 Fcitx：通过。覆盖普通模式零推理、手动生成 / 翻译、稳定前缀、取消、SSE 完成 / 截断 / 中断、源修订 / 配置重载淘汰旧采纳、Tab / 自定义 / 禁用 / repeat / key-up、preedit 优先、失焦、Password / Sensitive 和 `commitString`。
- 真实 CPU GGUF：通过上述校正、续写、翻译及普通零进程 / 共享实例 / 缓存 / 卸载检查。
- 从安装包解包的 addon 和数据运行原有 `rimes-buffer-fcitx-e2e`：通过 Buffer 输入、分块发送、焦点 / 密码、长按 Return 与残留 repeat 回归；未安装该包。
- 迁移目录后重新生成 CMake 缓存并构建相关目标，最小 CTest 通过；Git 根目录、worktree 与 origin 均指向新的开发检出 / 指定远端。
- `git diff --check`：通过。未运行远端 CI，提交使用 `[skip ci]`。根据用户后续明确指令，从开发目录提交并推送源码；无远端 Release 或安装包上传。

## 目标是否达到与剩余限制

文件和峰值内存预算达到。4 线程的**少量预热样本**达到首次可见约 300ms / 完整约 1 秒目标；2 线程续写首次可见 338ms 略超目标。权重加载后的第一次聊天存在提示词预填充，真实 Fcitx 首次校正预览 456ms，未达到 300ms；冷启动不会承诺该预热延迟。

模型的无错中文短句有时不给续写，仍需更多质量评估。长文本、连续高频输入、多语言、不同应用和长时间使用尚未广泛验收；长单元可因 96 token 输出或上下文预算而被拒绝。当前未提供自动长文拆分、任意应用快捷键冲突检测或建议采纳撤销界面。

本轮按约束未替换运行中的输入法，未进行当前真实桌面宿主的长期打字验收，未连接真实云端 AI，未扩展 Android / Windows，也未恢复 Apple 专用代码。

## 本地产物

位于 `/home/henry/Projects/Rimes/platforms/linux/ime/dist/`：

- `fcitx5-rimes-0.3.0-1-x86_64.pkg.tar.zst`：源码构建的输入法、renderer、设置与 model-host。
- `rimes-local-model-qwen3-0.6b-q4km-cpu-x86_64.tar.zst`：GGUF、CPU runtime、manifest、许可证和模型使用说明。

[大小与 SHA-256](cachyos-local-ai-20261012/artifacts.json)。两者被 Git 忽略，仅作为本地产物。使用见 [AI.md](../../platforms/linux/buffer/AI.md)。更新由用户自行执行 `sudo pacman -U`，下次登录加载，当前会话不自动升级。
