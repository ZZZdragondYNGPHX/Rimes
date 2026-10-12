# Linux 架构

主要目标：CachyOS x86_64 / KDE Plasma / Wayland。Android 和 Windows 保留独立工程。

`platforms/linux/ime/src/fcitx` 是系统 Fcitx5 加载的 C++ addon。
`src/engine` 封装 librime、键符映射、快照、路径与用户目录锁；每个 InputContext 拥有独立会话。
字典部署在维护线程进行，部署期间按键放行，完成后回到 Fcitx5 事件循环。

普通文本、Buffer 与 Capsule 最终通过当前上下文的 `commitString` 投递。
保持 preedit，校验焦点与上下文身份；密码框和失效目标不可捕获或投递，不使用剪贴板或模拟按键注入文本。

BufferService / CapsuleService 位于 addon 内；GTK3 companion 通过当前用户 runtime
目录的 Unix socket 接收模型并返回动作。进程退出或重启可丢弃临时 Buffer；持久笔记和学习词库保留。
Wayland 面板使用 gtk-layer-shell，X11 使用现有定位逻辑，不把 AppKit/IMK 行为当成 Linux API。

默认程序前缀 `/usr`，addon 遵循系统 Fcitx5 的 CMake 安装目录。
共享方案 `/usr/share/rimes/data` 由审核过的数据闭包 staging 产生。
用户数据 `$XDG_DATA_HOME/rimes`、日志 `$XDG_STATE_HOME/rimes/log`，默认分别为
`~/.local/share/rimes` 和 `~/.local/state/rimes/log`；与 stock fcitx5-rime、IBus 隔离。

`Shared/Engine` 保存 Android 后续构建仍需的非 Apple 词库、许可和依赖锁。
`Shared/Sources/RimesCore` 是平台无关算法参考，Linux 运行时使用 C++ 核心，不依赖 Swift。
`OfficialPlugins` 是去除 Apple 适配后的上游源码快照，来源记在 `UPSTREAM.json`，
`plugins.lock.json` 校验保留文件。导入脚本拒绝 Apple 原生来源和目标，防止重新生成已删除源码。

实现与已知限制见 [Linux IME](platforms/linux/ime/README.md)、[Buffer](platforms/linux/buffer/SPEC.md)、[Capsule](platforms/linux/capsule/SPEC.md)。

## AI 与翻译

`platforms/linux/buffer/ai` 封装私有服务配置、libcurl 网络工作线程与 source-ID 绑定的结果工作区。
输入与网络线程分离，完成回调回到 Fcitx5 dispatcher；来源修订、工作区、捕获目标与请求 epoch
共同拒绝迟到结果。翻译按原文 block 身份组成句子单元，稳定前段不随编辑尾段重新请求。
原文只在对应结果成功 `commitString` 后退休。手动复制结果不触发投递或消费。
GTK 设置是可激活的普通窗口，Buffer 仍是非激活 layer-shell 面板；设置保存不自动恢复旧焦点。
配置路径 `$XDG_CONFIG_HOME/rimes/ai.json`，目录 0700、文件 0600，不经 socket 或日志发送密钥。

0.3.0 的 `Suggestion` 冻结原文块、修订、目标与 UTF-8 范围。流式文本只是不可采纳预览；完成后从冻结原文推导校正范围或续写位置，拒绝整段改写和过期结果。Tab / 自定义键仅对有效完整建议生效；物理按键 latch 处理一次采纳及 repeat / key-up。BufferModel 原子替换局部范围，仍由既有发送路径投递。

`LocalRuntime` 仅在高级模式启动控制线程，异步运行 `rimes-model-host` / llama-server，AI 与翻译共用一个进程。普通模式不启动或推理；退出后可配置闲置期限卸载，未完成的加载直接取消。host 设置父进程退出信号；localhost API 使用随机端口与私有认证文件。模型、运行程序、来源和许可证在用户模型目录与独立本地产物中，不进入 Git。

请求工作线程只保留最新待处理任务，Fcitx 事件线程不等待网络或模型。句末立即触发、非句末默认 250ms，建议定时器使用 1ms 精度避免默认时间事件合并增加等待。设置显式选择 managed local / localhost API / cloud API；没有跨渠道回退。保护态包括 Fcitx Password 与 Sensitive。
