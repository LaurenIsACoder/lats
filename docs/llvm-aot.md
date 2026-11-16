# 离线 AOT 工具接口

LATS 负责生成普通翻译代码及其运行时执行。独立 LLVM AOT 工具负责函数编译和 AOT 文件组合。两个项目通过保存的代码页、普通 AOT 文件、运行时信息和公开头文件交换数据。

首版以生成代码效率及整套 SPEC int 的正确输出和性能对照为目标。默认 LLVM 正常 CALL/RET 不传播算术 flags，异常沿用近似现场；函数内真正消费的条件码及 helper 指令语义仍由编译器保证。显式精确恢复夹具不构成默认模式的接口承诺。

## LATS 修改范围

| 修改 | 主要文件 | 原因 |
| --- | --- | --- |
| 发布文件布局和构建信息 | `include/lats/`、`sys-aot-runtime-info.c` | 离线工具需要准确的磁盘结构和运行时字段位置，不能依赖内部头文件或源码文本 |
| 提供正常的离线命令 | `sys-aot-offline.c`、`sys-aot-tu-merge.c`、`softmmu/vl.c` | 在已初始化的 vCPU 上复用普通翻译器，生成调用、返回和异常回退所需的模板 |
| 装载 LLVM 函数及其依赖 | `sys-aot-format.c`、`sys-aot-load.c`、`sys-aot-reloc.c` | 校验恢复表、完成重定位，并将多个函数关联到普通 TU 的代码页 |
| 连接普通调用和 LLVM 入口 | `translate.c`、`sys-eob.c`、`sys-ras.c`、`sys-static-codes.c` | 普通调用者需要验证目标代码页、进入匹配的函数入口，并在未命中时继续普通执行 |
| 执行 LLVM 中的系统调用 | `sys-static-codes.c`、`sys-aot-format.c`、`sys-aot-load.c` | 共享入口保存现场并复用 `helper_syscall`，客体内核沿原有路径执行；返回用户态后由普通翻译继续，中间地址不生成 LLVM 入口 |
| 执行 LLVM 中的原子操作 | `sys-aot-atomic.c`、`cpu-exec.c`、`sys-static-codes.c` | 保存完整现场后调用既有原子访存 helper，正常完成返回 LLVM；故障、串行重试及代码失效交回普通调度；每个 LLVM 函数只有一个入口 |
| 调用 CPU 查询、x87 和系统 helper | `sys-aot-cpu-query.c`、`sys-aot-x87.c`、`sys-aot-system.c`、`sys-static-codes.c` | 使用独立宿主栈帧调用现有 helper，正常返回时继续当前 LLVM 函数；异常使用已提交的客体现场 |
| 维护浮点环境和快慢模式 | `sys-tr-fctrl.c`、`sys-eob.c`、`sys-ras.c` | 按已有 TB flags 选择模式；修改模式后清除返回预测并完整退出，避免返回旧模式的 LLVM 调用者 |
| 异步中断 LLVM 执行 | `sys-sigrt.c`、`dune-loop.c`、`native-fault-resume.inc`、`cpu-exec.c` | 通知并重试退出请求，保存活动 LLVM 机器现场，执行客体中断后继续原机器指令 |
| 恢复访存故障 | `hamt/native-recovery.inc`、`hamt/native-fault-resume.inc`、`sys-hamt-excp.c` | 默认模式保存宿主现场以继续执行；精确模式按故障机器指令恢复客体现场 |
| 管理函数地址和生命周期 | `cpu-exec.c`、`tb-maint.c`、`tcg/region.c`、`tcg/region-tree.inc` | 故障查找、代码页失效和代码区回收必须同时处理普通块与关联的 LLVM 函数 |
| 提供可选执行计数 | `translate-all.c`、`ir2.c` | 验收时区分普通翻译执行和真正进入 LLVM 函数，发布产物可关闭计数 |

普通翻译规则、TU 页面检查、调用缓存和 AOT 发布版本仍由 LATS 管理。离线工具提交机器码、重定位及恢复信息；LATS 在装载时验证这些数据，不能依赖工具曾经运行过某个测试。LLVM 库只出现在独立工具项目中。

LLVM 直接或间接 CALL 的目标尚未翻译时，冷查找在保存客体状态后可生成普通目标块。源和目标的代码页、执行模式以及返回栈状态仍有效时，沿原 CALL 进入目标，返回后继续调用者的 LLVM 代码；不能确认这些条件时沿用普通退出。普通目标之后遇到其他需要完整退出的事件时，仍可能结束挂起的 LLVM 调用。

`AOT_NATIVE_SYSCALL` 只用于长模式 CPL3 的 `SYSCALL`。加载器核对完整入口代码、源指令及其实际长度，再把终止分支指向共享入口。共享入口撤掉全部 LLVM 宿主栈帧、清空软件返回栈，按普通系统调用的约定保存 GPR、SIMD、标志和浮点控制状态，并调用既有 `helper_syscall`。之后回到普通分派器执行客体内核；SYSRET/IRET 根据实际返回地址和状态选择新的用户态入口。客体栈内容保留，挂起的 LLVM 宿主返回地址不再使用。

## 公开定义

安装时发布 `include/lats/aot-format.h`、`include/lats/aot-recover.h`、`include/lats/aot-atomic.h`、`include/lats/aot-cpu-query.h`、`include/lats/aot-x87.h` 和 `include/lats/aot-system.h`。

这些头文件定义 AOT 文件布局、精确恢复记录及校验函数，以及原子、CPU 查询、x87 和系统 helper 操作的描述和源指令校验函数。它们不暴露 `CPUState`、`TranslationBlock`、锁或运行时索引。磁盘布局使用 64 位小端宿主，保留现有 AOT 格式和已有重定位编号。

`target/i386/latx/include/sys-aot-format.h` 和 `latx-native-recover.h` 为 LATS 内部入口。离线工具包含已安装的公开头文件。

## 运行时信息

```sh
lats-x86_64 --aot-runtime-info
```

该命令不初始化虚拟机。JSON 包含实际可执行文件 SHA-256、AOT 版本、客体寄存器环境中所需字段的偏移、返回栈大小、中断请求偏移，以及常见 64 位 Linux 用户代码的默认执行标志。启用 profiler 时额外输出 LLVM 入口计数器偏移。

字段偏移由同一次构建的 `offsetof` 产生。工具必须校验实际运行时文件的摘要，不能从 LATS 源码文本提取内部结构布局。代码页对应的准确执行模式优先使用保存的普通 AOT 或实际执行信息；默认用户标志不能代替任意客体系统的运行状态。

`native_async_exit: true` 表示运行时已在 `intopt=3` 下支持 LLVM 主体的异步退出与继续执行，包括无普通出口的函数内循环、REP 循环和活动 LLVM 调用帧。该字段复用运行时功能声明方式，不修改 AOT 格式。未提供该声明的运行时不能用于绑定无中断检查的 LLVM 产物。入口要求 HAMT DMW、LASX 和 `rastoptp`，最多一个 vCPU，且每个 vCPU 使用独立宿主线程；多 vCPU 配置保留普通翻译。

`native_fp_environment: true` 表示 SSE 浮点环境由 LATS 维护。离线工具只将直接浮点运算绑定到没有 `HF_SIGFPE_ON` 的普通 TB；已有 `tb_lookup` 及调用目标检查负责模式选择，LLVM 入口和运算不额外判断模式。`LDMXCSR`、`STMXCSR`、`FXRSTOR` 和 `XRSTOR` 使用普通翻译。修改模式的指令结束当前 TB、清除返回预测并丢弃挂起的 LLVM 调用帧，然后以新 flags 查找下一条指令；即使旧调用者没有宿主栈帧，也不能保留其返回预测。

舍入方式和硬件异常标志沿用现有物理 FCSR 处理。DAZ/FTZ 没有独立 TB flags，本版沿用普通 LATS 的语义限制；LLVM 不添加软件 DAZ/FTZ/DE 模拟。该运行时声明不改变 AOT 格式或缓存版本。

运行时复用 [LATS PR #122](https://github.com/NiuGenen/lats/pull/122) 的退出请求发布和定时重试机制。LLVM 内部跳转不属于普通 TU 跳转记录，KVM 退出时另行捕获其寄存器和宿主栈。当前普通翻译在 `intopt=3` 下保留原有块末和回边检查，尚未合入 PR #122 的普通 TU 出口重写；LLVM 不生成这些检查。

入口发布和查找校验实际 `intopt=3`。保存文件尾部的 `interrupt_mode` 记录完整模式，使用原有对齐空隙，不移动已有字段或增加 AOT 版本号；装载和离线绑定拒绝不同模式的模板，避免原有布尔选项将 `intopt=2` 与 `intopt=3` 混同。

`native_syscall: true` 表示运行时提供 LLVM 系统调用共享入口。包含 `SYSCALL` 的函数必须要求该字段；旧运行时无法重定位这个入口，不能仅凭相同 AOT 版本接受产物。

`native_hamt_atomic: true` 表示运行时支持 `lats/aot-atomic.h` 中的 HAMT 原子故障序列。LLVM 计算地址、结果和 flags，使用宿主 AM 指令直接执行对齐 RAM 操作；8/16 位逻辑操作与 NEG 的 CAS 重试仍由 LLVM IR 编译。该快路径不调用共享入口，不写原子请求，也不需要返回式 helper 的状态保存。LLVM 产物装载仍要求已有的 HAMT 和 DMW 条件。

固定原语返回旧值和访存完成标记。HAMT 页表处理成功时直接重试 AM；失败时只在已登记 LLVM 代码范围内识别 SDK 约定的完整六条序列，将返回 PC 移至 AM 后的失败分支。该处理发生在精确恢复、MMIO 重翻译和缺页挂起之前，不按普通翻译的固定寄存器分配保存 LLVM 临时值。失败分支把地址及原始操作数写入 `native_atomic_offset` 所指的已有请求，通过原有终止型原子入口放弃全部 LLVM 帧，再处理未对齐、完整读写权限和代码页失效。CMPXCHG 比较失败通过 AMADD 0 检查写权限，成功比较与访存完成标记分别判断；检查失败时保留指令前状态进入同一慢路径。

兼容的 `AOT_NATIVE_ATOMIC` 返回式入口继续接受 `native_atomic_offset` 和 `LATS_ATOMIC_RETURN` 请求。该请求包含地址、源值、指令描述及完成计数，沿用已有状态保存、异常和失效处理；新编译器的 RAM 快路径不使用它。profiler 的 `LLVM atomic helper` 只统计这个共享入口的完成次数，不能当作全部 LLVM 原子的执行次数。

支持 8/16/32/64 位 LOCK ADD、OR、ADC、SBB、AND、SUB、XOR、CMPXCHG、XADD、INC、DEC、NOT、NEG 及隐式原子 XCHG，以及 16/32/64 位 LOCK BTS/BTR/BTC 和 LOCK CMPXCHG8B。位索引和字地址由 LLVM 计算，慢路径接收位掩码；CMPXCHG8B 使用成对寄存器比较并只修改 ZF，其描述不会进入普通 CMPXCHG64 的返回式快捷路径。小宽度操作不扩宽访存；未对齐和跨页由既有普通原子处理检查完整权限后提交。多 vCPU 竞争、设备 MMIO 和宿主并发修改的共享内存尚未验收。

PAUSE 在目标配置下直接省略。RDTSC 直接读取宿主 `rdtime.d`，乘以运行时信息的 `tsc_multiplier`，加上客体环境的 `tsc_base_offset` 与 `tsc_offset`，分别零扩展低、高 32 位写入 RAX/RDX；不经过共享入口或保存其他寄存器。两个 offset 字段在 JSON 中表示对应环境成员的字节偏移，倍率在普通 LATS 选项初始化后取得，未启用 `CONFIG_TSC_FREQ` 时为 1。启用该构建选项时，运行时信息须从部署主机取得，缓存与普通 LATS 一样不承诺跨不同倍率的主机使用。装载 LLVM 主体要求已有 PAUSE/RDTSC 优化开启，不增加执行时配置判断。

`native_cpu_query: true` 表示提供 `AOT_NATIVE_CPU_QUERY` 返回入口，保留 `CPUID/RDTSC/XGETBV/PAUSE` 描述兼容；当前编译器仅将 CPUID/XGETBV 送入该入口。`native_x87: true` 表示提供 `AOT_NATIVE_X87` 返回入口；扩展描述覆盖超越函数、余数、BCD、FISTTP、大整数加载及条件整数转换等原 LATS 同样使用 helper 的操作。第三参数为内存地址或已经读取的整数，存储成功后才弹栈。实际装载仍检查 `soft_fpu` 和完整源指令。

`native_system: true` 表示提供 `AOT_NATIVE_SYSTEM` 返回入口，支持 SSE4.2 PCMP*STR*、LAR/LSL/VERR/VERW、未加锁 CMPXCHG8B/16B。描述包含长度、操作、寄存器、宽度和控制立即数。LLVM 对段选择子内存源仅读取 2 字节；PCMP 内存源由 helper 加载至局部值，不借用客体 XMM 寄存器。LAR/LSL 仅在成功时写目的寄存器。

这些入口先提交 PC、GPR、XMM、整数标志和浮点状态，再调用原有 helper。CPU 查询描述通过 r4 传入；x87 和 system 的操作数通过 r4、描述通过 r5 传入。独立宿主栈帧保存返回地址，返回时 SP 保持 LLVM 所需的直接映射地址，不压入客体返回地址。system 返回后重新导入整数 flags。发生异常时使用已提交现场；存储引起代码失效时在完成指令后退出，按下一条客体 PC 重新选择入口。装载器核对专用桩代码、源编码和描述。

普通 x87 快路径由 LLVM 直接翻译，采用默认 `soft_fpu=1`、`soft_fpu_fast=0xfffffff` 的 80 位表示及 double 算术精度。运行时信息提供 `fpus/fpuc` 和必要的软浮点异常字段偏移，复用已有 `nofp/fpstt/fptags/fpregs` 布局。x87 与 MMX 互转只同步当前有效表示，不改变原有标签或栈顶。控制环境修改、环境保存恢复及初始化由普通 LATS 处理。设备 MMIO、未屏蔽浮点异常及跨页部分写入顺序尚未验收。`RDSSP` 保留当前普通翻译的空操作行为；`XBEGIN/XABORT/XEND` 使用当前普通翻译的非法指令异常模板，不提供事务执行。

## 普通翻译命令

下表中的 `QEMU_OPTIONS` 包括与部署一致的机器、CPU、HAMT、TU、RAS 和优化选项。普通翻译在初始化后的 vCPU 上执行，直接调用 LATS 的现有翻译器。

| 命令 | 输入和输出 |
| --- | --- |
| `--compile-code-pages PAGES MODE REQUESTS OUTPUT QEMU_OPTIONS` | 从完整保存页生成普通 AOT；`MODE` 为 `CS_BASE:FLAGS:CFLAGS`，三个字段均为 uint32，来自实际执行模式 |
| `--compile-function-code-pages PAGES MODE REQUESTS OUTPUT QEMU_OPTIONS` | 生成整函数的普通模板，将普通翻译产生的多个 TU 保存为一个源记录；首个请求必须是函数入口 |
| `--compile-div-code-pages PAGES MODE REQUESTS OUTPUT QEMU_OPTIONS` | 从保存页生成内存 DIV/IDIV 的直接异常模板 |
| `--compile-code-pages-batch JOBS.json QEMU_OPTIONS` | 初始化一次 vCPU，依次执行保存页的普通翻译、整函数模板和除法异常模板任务 |
| `--compile-function-pages INPUT OWNER REQUESTS OUTPUT QEMU_OPTIONS` | 使用已有普通 AOT 的代码页和模式，生成指定函数所需的普通模板 |
| `--compile-div-exception INPUT OWNER REQUESTS OUTPUT QEMU_OPTIONS` | 使用已有普通 AOT 生成直接除法异常模板 |
| `--retranslate-function-tu INPUT OWNER REQUESTS OUTPUT QEMU_OPTIONS` | 保留其他 TU，重译所选函数成员 |
| `--retranslate-saved-tu INPUT OWNER OUTPUT QEMU_OPTIONS` | 重译完整普通 TU |
| `--complete-saved-tbs INPUT OWNER REQUESTS OUTPUT QEMU_OPTIONS` | 补充指定普通翻译块 |
| `--partition-saved-tu INPUT OWNER REQUESTS OUTPUT QEMU_OPTIONS` | 将所选成员与保留成员分别翻译，保留其他 TU |
| `--partition-saved-tus INPUT REQUESTS OUTPUT QEMU_OPTIONS` | 一次分割多个普通 TU，保留未处理的普通和 LLVM TU |
| `--validate-saved-aot INPUT` | 仅检查磁盘布局；不等同于运行配置兼容性检查 |

`OWNER` 是保存的 TU 标识，不是函数地址。普通函数翻译请求每行是 `客体地址 字节数 指令数`。批量分割请求为现有 TU 标识和所选成员地址，保留源文件中的执行模式和页面依赖。所有输出使用新路径，拒绝覆盖已有文件。

保存页批量命令读取 JSON 数组，每项包含 `operation`（`ordinary`、`function` 或 `division`）、`pages`、`mode`、`requests`、`output`，后四项分别对应单任务命令的参数。所有任务使用同一组运行配置。每项输出关闭并通过布局和身份检查后，才打印 `CODE_PAGE_BATCH_DONE` 及从零开始的任务序号。某项失败时进程退出；调用者保留已完成输出，只重新启动未尝试的独立任务。已有输出文件本身不能证明任务成功。

`PAGES` 以八字节 `LTPAGES1` 开始，随后重复“小端 uint64 页地址 + 4096 字节页面”。页面必须完整、对齐且唯一，最多 `AOT_TU_MAX_PAGES` 页。指令跨页时必须同时提供两页。请求中的普通翻译块边界必须符合 LATS 的基本块与页边界要求；工具需要校验返回的实际字节数和指令数。

整函数模板保留每个普通片段的完整机器码、物理成员和重定位，包括调用后内联与独立保存的返回成员。合并仅调整 TU 内偏移、重定位所属范围及搜索数组位置，不重新编码分支，也不把普通片段当作 LLVM 函数。代码页数量按实际依赖计算，允许页面不连续。函数的总成员数可以超过单次普通 TU 生成的容量；每个普通片段仍遵守原生成限制。重复入口、冲突页面、不同执行模式和偏移超出可表示范围时拒绝输出。

`checktu` 的逐字节重译核对不适用于多个片段拼接成的整函数模板。模板成员数超过固定生成容量时，此检查明确报错退出。

直接除法异常模板只抛出异常，不重新读取内存除数，也不重新执行除法。编译器在已有除数和商溢出检查之后进入该模板。其他指令仍使用正常翻译规则。

## 运行时执行

`sys-aot-load.c` 校验并装载 LLVM 函数、建立函数与普通块的关系；`sys-aot-reloc.c` 按本次运行的地址完成重定位。普通 CALL/RET、返回预测和未命中入口由 LATS 维护。

LLVM 主体使用 32 字节对齐的代码基址。未启用树形代码索引时，装载器在 TBMini 查找表后补齐代码基址，并单独保留查找表原地址；普通 AOT 的布局不变。编译器负责保持主体在所属代码区内的相同对齐，装载校验仍接受原有的八字节对齐产物。

LLVM 函数与普通 TU 共用代码页依赖。任何依赖页变化都使对应函数失效。所有者中每个函数的本机地址区间单独登记，异常查找不会用相邻函数的恢复表。

发布入口前必须校验完整函数实际包含的全部源代码页。跨页共享尾部尚未映射时，首次入口仍使用普通翻译；代码页自然映射后可以重新进入 LLVM。本版不分段发布函数，也不为装载 LLVM 入口提前触发客体缺页。

装载或检查 LLVM 入口期间收到可处理中断或退出请求时，先返回调度器处理请求，再重新选择入口。被屏蔽的中断仍允许普通代码前进，避免反复重试而不能执行客体指令。离线工具通过完整退出交回尾跳转目标时，调度器从已发布的 LLVM 入口继续执行；需要重新执行原指令的异常出口仍使用普通翻译入口。

`hamt/native-recovery.inc` 按精确本机指令位置取得恢复点，恢复客体指令执行前的通用寄存器、XMM、标志位和访问地址。恢复表字段 40 保存 PF 布尔值，字段 42 保存用于计算 PF 的原始低字节，两者互斥；字段 32 保存用于判断 ZF 的完整结果。新运行时兼容两种 PF 表示，旧运行时会拒绝未知字段。恢复失败不会退用函数入口状态。普通 HAMT 处理随后负责缺页、写保护和指令重试。LLVM 不生成循环、REP 或尾跳转中的中断检查，中断退出由运行时异步处理。

没有精确恢复点的 CORE 使用宿主现场恢复：正常缺页进入客体内核前保存全部宿主寄存器、活动 LLVM 栈和返回状态，正常 IRET 后回到原机器指令。提供给内核的客体现场是近似值。异步中断复用相同的宿主现场保存方式。正常 IRET 的宿主返回帧仍有效、栈地址一致，且没有待处理请求或调试限制时，直接恢复其下的 LLVM 栈。其他情况先回到调度器，再经过正常翻译器入口建立新的宿主返回帧；不恢复已退出的 C 调用。新旧入口栈地址必须一致，否则明确失败。

每个 vCPU 只有一个挂起现场。CR3、用户权限或栈指针不匹配时，其他上下文继续普通单指令翻译；原上下文返回后才能恢复 LLVM。此机制不提供完整任务识别。挂起任务终止、exec、CR3 重用、修改返回上下文、挂起期间开启断点或单步、挂起代码失效、区域清空、二次挂起及超过 8 MiB 的活动栈不在首版支持范围。不能仅凭 CR3 变化丢弃快照，否则正常调度返回会重复执行 LLVM 已完成的操作。精确故障信号需要离线工具生成逐访存恢复点。CPU reset 和销毁会释放挂起状态。存在挂起现场时，运行时拒绝保存或迁移虚拟机。MMX 用户上下文在挂起期间的切换尚未验收。

MMX 直接翻译使用默认 `soft_fpu` 配置的 NOFP 状态。运行时信息通过现有字段偏移机制提供 `nofp`、`fpstt`、`fptags`、`fpregs` 位置及 `FPReg` 步长。离线工具沿用固定 F0..7，在函数边界同步计算结果；不新增调用参数或恢复表字段。普通出口进入 C 前保存有效的 MMX 值。共享 C 入口在私有栈记住进入前的有效标记，返回时恢复物理寄存器及标记，以支持普通块内的 helper 继续执行；查找未链接目标和 AOT 装载也遵守此规则。

默认挂起恢复可保留 LLVM 临时寄存器中的 MMX 值，向客体内核提供的 MMX 现场仍可能较旧。修改信号返回现场中的 MMX/x87 数据、标签或栈顶尚未验收；现有恢复检查不比较这些字段。本版不提供精确的异步信号 MMX 上下文。

profiler 构建可通过 QMP `x-query-jit` 查询 LLVM 入口执行次数。离线工具可选择插入这些计数；普通发布产物不需要插入。

## 构建和验证

在支持 HAMT 的 LoongArch 主机构建 `x86_64-softmmu`，启用现有 LATS 优化选项。构建 LLVM AOT 不需要向 LATS 链接 LLVM 库。`meson install` 安装公开 SDK 头文件。

系统正确性验收由独立 LLVM AOT 项目的 `tests/run_system.py` 执行，覆盖普通 LATS、LLVM 装载执行、异常恢复、中断和代码失效。生成普通模板和运行产物时应采用相同的宿主地址布局；现有 AOT 会校验辅助函数地址，可用 `setarch loongarch64 -R` 启动 LATS。

`python3 tests/tcg/i386/latx-aot/test-native-indirect-frame.py` 检查实际生成的间接 CALL 解析代码，核对进入共享 C 入口后目标地址、输出区和原始 SP 的栈位置。该检查可在编译主机运行；系统执行由 LLVM AOT 项目的 MMX 和完整系统测试验证。

## 已执行 CALL 的入口记录

`aot_block.misc_flags` 的 `AOT_BLOCK_OBSERVED_CALL_ENTRY` 表示客体 CALL 曾到达这个成员的精确起始地址。本项目标准运行配置（`tumode=both,ras=on,rastoptp=on,intopt=3`）下，新鲜录制的间接 CALL 在首次查找时设置它；NJC 和 FSHT 命中也检查是否已记录。尚未翻译的目标由下一次调度按地址和完整执行模式匹配后记录，异常或实际中断转移会丢弃待处理记录。JMP、RET 和 native ABI 入口选择不产生这项证据。保存重译只在同一记录入口继承该位，相同代码与模式的缓存合并对该位取并集。

离线工具可将这些地址作为函数分析起点，仍须验证完整控制流和返回栈。该位不表示间接跳转目标集合完整，也不改变函数 ABI。加载后的普通 AOT 和 LLVM 主体不增加采集检查。

采集限于默认用户态快模式；带非零 IOPL 等额外 EFLAGS 模式的调用沿用慢路径，不作为本版入口补证来源。入口补证要求从空缓存重新录制；已加载 AOT 不会因新增观察而重新保存。现有保存器仍排除原始跨页 TB，因此入口的首个 TB 横跨两页时可能没有这项记录。函数后续跨页由既有代码页恢复规则处理。
