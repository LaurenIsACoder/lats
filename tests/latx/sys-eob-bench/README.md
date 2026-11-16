# 系统态 EOB 判断测试

EOB 判断决定当前指令之后是否结束翻译块。本测试比较修改前后的生产函数，
同时检查返回值和整个 `TRANSLATION_DATA`，再测量判断吞吐。
使用真实 CPU、IR1、翻译数据结构、IR1 辅助函数和仓库内置 Capstone。

需要一个已完成构建的原生 `x86_64-softmmu` LATS 构建目录；
`build.py` 从其 `compile_commands.json` 读取生产编译参数。
它只编译测试，不启动或管理翻译器。

在仓库根目录准备修改前源码并构建：

```sh
git show e325cb9ec6dd8fb1bdf6f4b8bacbdf8aa5663837:target/i386/latx/translator/sys-eob.c > /tmp/eob-baseline.c
python3 tests/latx/sys-eob-bench/build.py build /tmp/eob-baseline.c /tmp/eob-test
/tmp/eob-test/bench verify
/tmp/eob-test/bench base hot 10000
/tmp/eob-test/bench new hot 10000
```

旧函数、新函数及原始 `ir1.c` 分别编译，关闭跨编译单元优化。
测试只提取 `sys-eob.c` 中判断函数所在的前半部分，其函数体保持原样。
`manifest.json` 保存源文件 SHA256、编译选项及构建目录。
构建目录的源码与配置须对应当前工作树。

`verify` 包含：

- 全部指令编号，以及 MOV/POP 的全部寄存器编号和寄存器/内存操作数类型。
- 保护模式、vm86、16/32 位代码段条件的八种组合。
- 单步、禁止中断、指令计数、REP 字符串拆分、跨页、命中/未命中断点、观察点及 PAUSE 设置。
- 翻译状态初值为零与一，检查未命中条件时是否保留状态。
- 使用 Capstone 在 16/32/64 位模式解码的实际指令样本。
- 对 INVPCID、浮点恢复、控制/段寄存器、单步和调试检查顺序给出独立预期值。

枚举指令和操作数的检查也包含不能组成实际机器指令的组合，用来覆盖判断条件；
实际解码样本另行检查。测试不执行客体指令，也不验证操作系统的断点交付流程。

计时命令为 `base|new hot|mixed passes`：

- `hot` 从 15 种常用整数、访存及控制流样本均匀随机选取。
- `mixed` 从包括系统指令、段寄存器、浮点模式切换和字符串操作的全部样本中选取。

每组输入为固定种子的 4096 条实际解码 IR1。先预热 16 遍，计时只包含判断调用
和输出写入，结束后逐项检查输出。`ns_per_op` 越低越好，
`million_ops_per_s` 为每秒百万次判断，越高越好。
以基线为 100 的分数可按 `基线耗时 / 当前耗时 × 100` 计算。

输入是合成混合，不代表 Java 的实际指令频率。运行方应在同一台机器交替测量，
固定 CPU 并检查同物理核另一线程的负载。小测试收益不能直接推算为 Java 总体收益。
