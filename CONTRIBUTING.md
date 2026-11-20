# 贡献指南 / Contributing to LATS

LATS 欢迎缺陷修复、兼容性改进、性能优化、测试和文档贡献。提交前请先搜索已有
Issue，避免重复工作；较大的功能或架构调整建议先提交 Issue 说明目标、方案和
影响范围。

LATS welcomes bug fixes, compatibility improvements, performance work, tests,
and documentation. Search existing issues before starting. For a large feature
or architectural change, open an issue first to discuss scope and design.

## 开发流程 / Workflow

1. Fork `lat-opensource/lats`，并从 `main` 创建独立主题分支。
2. 保持变更聚焦，避免在同一 Pull Request 中混入无关重构。
3. 在支持 LoongArch 的环境中构建并运行受影响测试。
4. 从个人 Fork 向上游 `master` 提交 Pull Request，并说明验证结果。

```console
git clone https://github.com/YOUR_ACCOUNT/lats.git
cd lats
git remote add upstream https://github.com/lat-opensource/lats.git
git fetch upstream
git switch -c TOPIC_BRANCH upstream/main
```

1. Fork `lat-opensource/lats` and create a topic branch from `main`.
2. Keep each pull request focused and avoid unrelated refactoring.
3. Build and test the affected functionality on a LoongArch-capable host.
4. Open the pull request from your fork to upstream `master` and describe the
   validation performed.

## DCO 签署 / Developer Certificate of Origin

每个提交都必须包含 Developer Certificate of Origin 1.1 的
`Signed-off-by` 尾注。签署表示你确认有权按本项目的 GPLv2 兼容许可证提交该
贡献。

Every commit must include a `Signed-off-by` trailer for Developer Certificate
of Origin 1.1. The sign-off certifies that you have the right to submit the
contribution under a GPLv2-compatible license.

```console
git commit -s -m "component: concise description"
```

`Signed-off-by` 中的姓名和邮箱应与提交作者一致。自动化工具或 AI 助手不得在
未经作者明确确认和授权的情况下代为添加 `Signed-off-by`。

The name and email in the sign-off must match the commit author. Automation
and AI assistants must not add a sign-off without the author's explicit review
and authorization.

## 构建与测试 / Build and Test

```console
./latxbuild/build.sh -c def
python3 tests/tcg/i386/latx-aot/ras-tp-bench/run-regressions.py
```

并非所有测试都能在 x86 主机运行。涉及系统翻译、HAMT、AOT 和客户机启动的
变更需要在 LoongArch 主机上验证；如果无法执行某项测试，请在 Pull Request
中明确说明原因。

Not every test can run on an x86 host. Changes involving system translation,
HAMT, AOT, or guest boot need validation on a LoongArch host. If a test cannot
be run, state the reason in the pull request.

## 许可证 / Licensing

提交的代码必须与本项目现有许可证兼容。新增第三方代码时，请保留原始版权和
许可证信息，并同步更新 `NOTICE`。请勿提交无授权的专有代码、二进制文件或
测试数据。

Contributions must be compatible with the project's existing licenses. Preserve
original copyright and license notices when adding third-party code, and update
`NOTICE` as needed. Do not submit unauthorized proprietary code, binaries, or
test data.
