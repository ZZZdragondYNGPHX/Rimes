每阶段及任务完成时，只在本地执行最小相关验证。

开发仓库在 `/home/henry/Projects/Rimes`；`/home/henry/Software` 是个人软件目录。开发、Git 提交和远端推送均从开发目录执行，不在个人软件目录新建开发检出。

# Repository instructions for coding agents

## Contribution attribution

- Add a `Co-authored-by` trailer to a commit when an AI coding agent materially designed, implemented, tested, or reviewed that commit.
- Use only a stable public identity that has been verified against the provider or its GitHub account. Do not invent names or email addresses.
- For Codex, use `Co-authored-by: Codex <267193182+codex@users.noreply.github.com>`.
- Credit temporary child or review agents through their parent agent unless the child has a persistent public identity of its own.
- Do not credit an agent that did not participate in the commit.
- Do not rewrite already-pushed history solely to add attribution unless a maintainer explicitly requests it.
- Keep the public agent list and attribution policy in `CONTRIBUTORS.md` current when a new coding-agent family materially contributes.
