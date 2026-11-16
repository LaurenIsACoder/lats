# 贡献者 / Contributors

LATS 感谢每一位贡献者。项目历史在开源整理时被合并为单个初始化提交，因此
GitHub 提交图和 Contributors 页面无法完整体现开发阶段的个人贡献。本文件记录
主要贡献者和贡献领域；名单排名不分先后。

Development history was squashed into a single initialization commit while
preparing the public repository. The GitHub commit graph and Contributors page
therefore do not fully reflect individual work from earlier phases. This file
records the principal contributors and their areas of work, in no particular
order.

## 主要贡献者 / Major Contributors

- **Lu Zeng** — 项目发起人，负责整体设计、架构方向、性能优化方案与项目推进。
- **Gen Niu** — 项目维护者，主要负责 HAMT、跳转链接和系统模式性能优化。
- **Hanlu Li** — 项目维护者，主要负责 RAS、代码缓存空间及多项系统模式优化。
- **Chaoyi Liu** — 指令模式优化与指令级测试。
- **Wenqiang Wei** — AOT 功能、TU 及系统级预翻译。
- **Rengan Yue** — SoftFPU 和 TU 相关优化。
- **Jun Yi** — 原子指令、系统调用及相关系统模式优化。
- **Zeqian Xue** — 浮点异常、向量指令扩展及相关优化。
- **Xiaotian Wu** — LoongArch ABI 2.0 和工具链适配。
- **Zhaoyang Xie** — AES、PCLMULQDQ 和浮点指令翻译优化。
- **Yanzhi Lan** — 访存、MOVNTI 和指令翻译优化。
- **Dongru Zhao** — AOT、x86 指令翻译和代码布局优化。

## 原型系统 / Prototype System

以下开发者参与了 LATS 原型系统、HAMT、Dune 适配和早期兼容性工作：

- **Gen Niu** — 原型系统负责人。
- **Xueshi Hu** — Dune 在 LoongArch 上的适配。
- **Cun Li** — HAMT 方案创始者。
- **Wenjin Li** — HAMT 优化。
- **Xinyu Li** — SoftFPU 模式和 x86-64 支持。
- **Zeqian Xue** — 向量指令扩展支持。

## 其他贡献者 / Additional Contributors

- **Jing Li** — virtio-gpu、GTK 和图形显示相关修复。
- **Wenjin Li** — 调试构建和可选优化修复。
- **Yongcheng Wu** — AOT 目录权限和选项处理修复。
- **Jinyang Shen** — TU 分割后的模式重建修复。
- **guoguangyao** — x86 PC-relative 翻译支持。
- **ykkkx** — CMOV、SETcc 和条件指令优化。

开源后的贡献以实际 Pull Request、Issue 和代码评审记录为准。若名单存在遗漏或
需要更正，请提交 Issue 或 Pull Request。

Post-open-source contributions are tracked through pull requests, issues, and
code reviews. Please open an issue or pull request if this list needs a
correction or addition.
