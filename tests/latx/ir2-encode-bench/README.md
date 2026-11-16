# IR2 编码正确性与吞吐测试

直接编译仓库中的 `ir2_assemble_new()` 和完整指令格式表，在支持 LSX 的
LoongArch 机器上运行。不启动翻译器，不执行编码出来的指令。

准备源码（在仓库根目录执行，基线须为修改前的编码文件）：

```sh
git show e325cb9ec6dd8fb1bdf6f4b8bacbdf8aa5663837:target/i386/latx/ir2/ir2-assemble.c > /tmp/ir2-baseline.c
python3 tests/latx/ir2-encode-bench/prepare.py /tmp/ir2-baseline.c /tmp/ir2-generated
```

`prepare.py` 提取真实编码函数、完整 `IR2_INST` 结构和指令表，生成四个独立编译单元：

| 参数 | 编码实现 |
| --- | --- |
| `base` | 修改前的函数 |
| `reduce` | 只改为 LSX 寄存器内归约，即把四个操作数字段合并为一个机器码字 |
| `table` | 只使用合并后的 64 字节编码表；包含原补丁中提前处理 `LISA_DUP` 的改动 |
| `both` | 当前仓库的修改后函数 |

生成目录自包含，可复制到 LoongArch 机器构建。以下编译选项保留本次 LATS
生产构建的关键设置；不要开启 LTO，以免编码调用被内联或合并。

```sh
gcc -O2 -g -mlsx -fno-tree-vectorize -fno-strict-aliasing -fwrapv \
    -fno-stack-protector -fno-pie -no-pie -fPIC -std=gnu11 \
    -Wall -Wextra -Werror -I /tmp/ir2-generated /tmp/ir2-generated/*.c -o /tmp/ir2-bench
/tmp/ir2-bench verify
/tmp/ir2-bench base hot 50000
/tmp/ir2-bench both hot 50000
```

`verify` 对所有具有机器码的指令以及 `LISA_DUP`，逐操作数测试边界值、
每一个置位位置及固定种子随机输入。参考编码器按原始格式表逐位插入字段，
不使用待测的合并表或向量归约。四个版本均须匹配参考结果，否则返回非零。
输入含超出字段位宽的值，用于检查原编码器保留低位的行为，不代表接受非法指令。

计时命令为 `base|reduce|table|both hot|wide passes`。每遍编码 4096 条真实布局的
IR2 指令，先预热 16 遍，计时只包含编码调用和输出写入。结束后逐个检查输出。
`million_ops_per_s` 为每秒百万次编码，越高越好；`ns_per_op` 越低越好。
同一输入下可用 `新吞吐 / base 吞吐 × 100` 得到以基线为 100 的分数。

- `hot`：30 种常用整数、访存、分支指令及 `LISA_DUP` 的均匀随机混合。
- `wide`：全部可编码指令的均匀随机混合。

两组均为固定种子的合成输入，不能代表真实 Java 指令频率。
应在同一台机器上交替运行多个版本，固定 CPU 并检查同物理核的另一线程是否空闲。
这些运行安排由调用方负责。`manifest.json` 记录生产源码和表的 SHA256。
本测试只能证明编码器本身的正确性和吞吐变化，不能证明 Java 总运行时间下降。

## 输出循环与异常恢复记录

另外使用真实的 TB/TU 输出循环测试默认配置的直接调用及日志判断消除。
基线与新循环都使用当前编码器，因此测得的是输出循环的增量收益。
需要已完成构建、源码对应当前工作树的原生 `x86_64-softmmu` 构建目录：

```sh
mkdir -p /tmp/ir2-output-base
git show 2aaedee25f:target/i386/latx/translator/sys-translate.c > /tmp/ir2-output-base/baseline-sys-translate.c
git show 2aaedee25f:target/i386/latx/optimization/sys-tu.c > /tmp/ir2-output-base/baseline-sys-tu.c
python3 tests/latx/ir2-encode-bench/build-output.py build /tmp/ir2-output-base /tmp/ir2-output-test
/tmp/ir2-output-test/output verify
/tmp/ir2-output-test/output-profile verify
/tmp/ir2-output-test/output base tb 100000
/tmp/ir2-output-test/output new tb 100000
/tmp/ir2-output-test/output base tu 100000
/tmp/ir2-output-test/output new tu 100000
```

脚本沿用生产编译参数，并生成普通版和启用 `CONFIG_LATX_PROFILER` 的版本。
新旧输出循环、两种编码器、`fix_tb_info()` 及异常恢复记录编码均使用生产函数。
测试提供标签预处理与记录缓冲区分配的接入点，比较机器码、完整翻译数据、
TB 元数据、逐条指令的位置记录、实际编码的异常恢复记录及日志文本。
标签预处理未在本次改动中修改；输入分支已解析为偏移，标签标记由循环跳过。
日志中的指令详情打印由测试函数记录调用，不测试指令详情打印函数自身。

检查覆盖旧/新编码器、日志开关及地址过滤、单 TB 和多个 TB 的 TU、
profiler 标记、无 TB 的序言/尾声、缓冲区不足、标签预处理重试、
连续/独立记录缓冲区以及记录分配失败。
每个版本执行 3072 组新旧对比，比较范围包括异常恢复依赖的全部状态。

计时只对默认新编码器、日志关闭的配置进行。`tb` 为一个 TB 的 128 条客体指令，
`tu` 为四个 TB，各含 32 条客体指令；每条客体指令产生 8 条宿主指令。
测量包括输出写入、位置记录、缓冲区检查；TU 还包括真实的异常恢复记录编码。
标签解析和缓冲区分配由测试接入点代替，不能将这个得分当作完整翻译耗时。
