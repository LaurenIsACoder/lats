# SoftRAS 回归与性能测试

本目录提供 CALL/RET、嵌套返回、syscall、浮点状态、多 vCPU、失效重链接及 SPEC CPU2000 整数测试。性能统计完整循环的一次 CALL/RET，包含 guest 栈访问、callee 和循环控制。

## 统一回归入口

在仓库根目录运行，需 Python 3.11 及以上、C 编译器和两个可用物理核心：

```sh
python3 tests/tcg/i386/latx-aot/ras-tp-bench/run-regressions.py
```

18 项脚本覆盖 PTR 解码、失效入口、代码区分配、C 边界中断、栈保留与清理、链接生命周期、TU/AOT 重定位、缓存格式、测量协议和 QMP 超时清理。独立测试保留在上一级目录。

## 测试环境与 fixture

性能和系统运行回归在 LoongArch HAMT 主机执行。需原生 LATS system 二进制、GDB、objdump、静态 C 编译器及私有 cache mount namespace 的 sudo 权限。`--root` 指向测试环境目录，包含 `runtime/vmlinuz-4.4.0-186-generic-x64`、`runtime/pc-bios/`、本目录的 `scripts/`、x86 和 LoongArch fixture。

以下命令从本目录执行。在 x86_64、LoongArch 主机分别构建对应二进制；`--cc` 可指定交叉编译器：

```sh
cd tests/tcg/i386/latx-aot/ras-tp-bench
python3 scripts/build-fixture.py direct --arch x86_64 --out /absolute/run
python3 scripts/build-fixture.py direct --arch loongarch64 --out /absolute/run
```

将两个主机生成的二进制和 initrd 放到同一个运行目录。在 LoongArch 主机准备驱动、guest 内核和 BIOS：

```sh
mkdir -p /absolute/run/runtime
cp -a scripts /absolute/run/
cp /absolute/vmlinuz-4.4.0-186-generic-x64 /absolute/run/runtime/
cp -a /absolute/pc-bios /absolute/run/runtime/
```

fixture 可选 `direct`、`indirect`、`chain`、`fp`、`multi`、`smc`。x86 构建同时生成 `bench.cpio`。`direct` 包含 direct、单目标 indirect 和四目标 indirect；`indirect` 验证内存 CALL、RSP 寻址及目标槽被压栈覆盖；`chain` 包含 4～600 层返回和链底 syscall；`fp/multi/smc` 分别验证浮点状态、并行返回和跨 CPU 修改代码。替换 fixture 时同时更新 x86、LoongArch 二进制和 initrd。

## 暖 CALL/RET 微基准

测量线程和控制线程固定到不同物理核心；先用 `lscpu -e=CPU,CORE` 核对拓扑。下面使用 CPU4/5 的物理核心测量、CPU2 控制：

```sh
sudo -n --preserve-env=HOME taskset -c 2 python3 scripts/run-tp-ras-compare.py   \
  --root /absolute/run \
  --binary /absolute/lats-x86_64 --out /absolute/micro \
  --native-binary /absolute/run/bench-la \
  --modes disabled,legacy,tp --cases call_direct,call_indirect1,call_indirect4   \
  --vcpu-host-cpus 4 --rounds 9 --lengths 100000000,400000000 --counter-handshake --pmu --background-budget-percent 0.5   \
  --warm-code-snapshots --trace-start 0x4035f0 --trace-end 0x403920
```

三个模式分别为 NJC、原 SoftRAS、r2 SoftRAS，均与原生校验值比较。默认使用 JIT TU、测试显式关闭 AOT，其余产品默认参数保留。九轮轮换模式及长度顺序，以两个长度的计数差计算每次开销，取中位数；GO/STOP/NEXT 握手圈定 PMU 窗口，并核对计数器未复用。快照检查实际暖代码字节与元数据不变，地址范围应对应本次构建的 fixture。

系统正确性运行可使用 `--smp 2/4`、`--guest-env RAS_GUEST_CPU=1/3`、`--flush-after-warmup full/mtcc` 和 `--cold-state-snapshots`。这些状态读取用于正确性验证，不混入上述暖性能统计。

## syscall 与 32 层返回

```sh
gcc -O2 -static -no-pie -mno-red-zone syscall-probe/main.c syscall-probe/probe.S -o /tmp/ras-probe-x86
python3 scripts/create-initrd.py /tmp/ras-probe-x86 /tmp/ras-probe.cpio
sudo -n --preserve-env=HOME taskset -c 2 python3 scripts/run-syscall-probe.py   \
  --root /absolute/run \
  --binary /absolute/lats-x86_64 --guest /tmp/ras-probe-x86   --initrd /tmp/ras-probe.cpio --out /absolute/syscall
```

独立汇编在 32 层链底执行 raw getpid。PRE/POST/OUTER 采样读取 top 与存活记录；POST 到 OUTER 仅执行 RET。正常和 helper 返回均验证 `35→35→3`，校验值 570。分别加 `--invalidate lookup/kernel/user` 验证查找缓存、内核代码区及用户代码区清理。

## SPEC 与 AOT

需合法安装的 SPEC CPU2000、静态 x86 整数程序，以及带 sh、echo、gzip、sha256sum 的静态 x86_64 BusyBox。`prepare.py` 在 x86_64 主机上从套件定义提取全部调用与输入，生成 initrd；程序、输入、输出和完整缓存保留在私有测试目录：

```sh
python3 spec2000/prepare.py --spec /absolute/spec2000 \
  --bin /absolute/specbin --busybox /absolute/busybox-x86_64 \
  --out /absolute/spec-fixture
sudo -n --preserve-env=HOME taskset -c 2 python3 spec2000/run.py   \
  --root /absolute/run --spec /absolute/spec2000 --fixture /absolute/spec-fixture   \
  --binary /absolute/lats-x86_64 --out /absolute/spec-ref   \
  --cpu 4 --sizes ref --vm-lifecycle fresh \
  --rounds 3 --guard-host-load --background-budget-percent 0.5
```

默认使用 JIT TU、AOT off，比较原 SoftRAS 与 r2 模式。`fresh` 为每项、每个模式及每次重试单独创建虚拟机，结束后关闭，再启动下一实例；按轮次和项目交替模式顺序。每项执行全部输入调用；输出传输校验 gzip、长度和 SHA-256，再用官方 specdiff 与逐文件容差检查。每项取三轮中位时间，12 项按几何平均计算内部对比分数；这是自定义 SYS 运行结果，不是正式 SPEC 成绩。

AOT 正确性使用 `--aot on --sizes test --cache-root /absolute/aot-cache`，依次保存、重新加载相同缓存。现有缓存包含绝对 helper 地址，两遍需 `setarch loongarch64 -R` 固定各自宿主进程地址。脚本等待保存子进程结束；这些设置不改变产品 TU/AOT 默认值。

## 测量争用与结果

`--pmu` 微基准自动检查目标 CPU 及 SMT 兄弟线程；SPEC 用 `--guard-host-load`。只豁免指定测量线程及其启动身份，其他固定竞争线程累计到 1ms 即拒绝；暂停或阻塞的 VM、native、AOT 线程可以通过。线程扫描之外还核对核心运行时间，以发现扫描间已退出的进程。

长期测量允许明示的后台 CPU 时间预算：每核心六个宿主时钟刻度、两端读取跨度及窗口时长的默认 0.1%。完整窗口在开始前和结束时判定，约 1/10 秒滚动窗口保留突发超限证据；普通宽亲和性线程仅累计可能在目标核心执行的采样段，预算为 max(10ms, 窗口时长的指定比例)。这不代表跑分误差小于 0.1%，低于计时精度的干扰仍可能漏检。

仅争用拒绝可以重试，每条最多三次；保留各次完整结果与证据，不平均被拒绝数据。校验、超时、协议和监测错误立即停止。原始进程证据权限为 0600，不杀其他任务的进程。最终结果需核对 `host_load_guard.status`、证据 SHA-256、二进制与配置身份、暖代码身份及官方输出校验；原始输出和缓存不进入仓库。

可用 `--background-budget-percent 0.5` 在测量开始前指定普通后台线程预算，最大为 0.5%；默认仍为 0.1%。整段和滚动窗口使用同一比例，固定竞争线程的拒绝条件不变。报告记录实际比例。SPEC 的观察窗口包含 guest 运行、输出传输和宿主校验，比 guest 计时窗口更宽。不同规则下的数据分开保存和汇总。

SPEC `--resume` 支持 AOT 关闭、每项使用新虚拟机且不复用缓存的测量。二进制、输入、驱动、启动脚本、BIOS、校验程序、参考输出、CPU、规则和其他参数必须相同。只跳过已完成的整对，未配对的一行连同旧元数据保存在 `raw-before-resume-N.jsonl` 和 `metadata-before-resume-N.json`，恢复后重测该整对。末尾写到一半的记录与未配对行一同保留后重测，中间损坏则报错。各次启动使用独立的 `runN/` 目录和未使用的备份编号，保留旧尝试；文件锁阻止两个测量同时使用同一结果目录。存在结果的目录必须显式使用 `--resume`，不会自动覆盖。
