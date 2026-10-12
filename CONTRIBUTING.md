# 贡献

本仓库由 [ZZZdragondYNGPHX](https://github.com/ZZZdragondYNGPHX) 和
[MIYIZIII](https://github.com/MIYIZIII) 共同开发，保留上游历史、许可证及贡献署名。
完整协作规则见 [协作约定](docs/collaboration.md)。

## 开始任务

- 开工前沟通目标、涉及目录和接口依赖；功能、重构及可能重叠的任务先创建
  [开发任务 Issue](https://github.com/ZZZdragondYNGPHX/Rimes/issues/new?template=task.yml)，指定一位主要负责人。
- 小修复和错字可以直接创建 PR。任务按需认领，不固定划分个人模块。
- 从最新 `main` 创建短期任务分支：`<类型>/<作者>/<任务简述>`。
  类型使用 `feat`、`fix`、`refactor`、`docs`、`test` 或 `chore`；作者使用 GitHub 用户名或约定简称。

```bash
git fetch origin --prune
git switch main
git pull --ff-only origin main
git switch -c feat/zzz/buffer-search
```

共享或并行开发的检出使用独立 worktree，避免切换另一项任务正在使用的工作目录。

## 提交与合并

- 每个 PR 聚焦一项任务，说明结果、改动范围、关联 Issue 和本地验证。
- 合并前同步最新 `main`；作者处理冲突并重新执行受影响部分的最小本地验证。
- 核心改动和未分类路径必须由另一人批准；明确允许自查的路径可由作者自查合并。
  [CODEOWNERS](.github/CODEOWNERS) 是路径清单；涉及核心行为时按协作约定申请互审。
- 所有改动通过 PR 合入 `main`，统一使用 Squash merge，提交标题采用 `feat:`、`fix:`、
  `docs:` 等前缀，并在最终提交中保留实际参与者的 `Co-authored-by`。
- PR 合并后自动删除远端任务分支；放弃或被替代的任务先保留所需代码，再关闭 PR 并删除分支。
  本地分支由作者清理，定期执行 `git fetch origin --prune`。

## 本地验证与项目约束

主要开发环境是 CachyOS / KDE Plasma / Wayland。先阅读 [ARCHITECTURE.md](ARCHITECTURE.md)。
每阶段及任务完成时，只在本地执行最小相关验证。文档与规则变更只检查对应文件、语法和差异。
代码变更按影响选择构建和测试；例如 Linux IME：

```bash
platforms/linux/ime/scripts/build.sh
ctest --test-dir platforms/linux/ime/build -R '<相关测试>' --output-on-failure
git diff --check
```

输入与焦点变更按需运行隔离的 Fcitx5 testfrontend；真实 KDE/Qt/GTK 宿主验收单独记录。
不以远端 CI 代替本地验证，不主动触发远端检查。现有自动 CI 不作为本协作规则的必需检查；
纯文档或规则变更可在提交消息中使用 `[skip ci]`，Squash 时保留该标记。
用户明确要求推送或发布时按其授权执行，其他任务不自动推送或发布。

投递使用 Fcitx5 `commitString`，每个上下文独立会话；更新保留用户词库与持久笔记。
Android/Windows 为后续目标，不为 Linux 变更执行整个平台构建。
AI 实质参与的提交按 [CONTRIBUTORS.md](CONTRIBUTORS.md) 添加已验证的 `Co-authored-by`。
