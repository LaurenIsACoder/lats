# BQL 竞争度量基线报告(T-216 / WI-1646)

日期: 2026-08-04
代码: lats main `9c66d4d5eb`(本地 rsync 到 3a6000,无 .git 构建,LATX_VERSION=nogit)

## 环境

| 项 | 值 |
|---|---|
| 机器 | 3A6000 4 核 8 线程 @ 2.0GHz, 15GB RAM |
| OS | AOSC OS, 内核 6.16.12-aosc-main-16k |
| 客户机 | buildroot x86_64(bzImage + rootfs.cpio),`-M pc -m 2048 -smp 4 -device edu` |
| lats 运行参数 | `-enable-sync-profile -nographic`,LATX_AOT=0,HAMT 默认开(SFB 已关闭) |
| 负载 | bql-stress 20s:2 线程 edu BAR0 MMIO 打点(写 0x08/读 0x00)+ 2 线程 mprotect TLB shootdown(64MB 共享区) |
| 构建 | 最小配置(禁 spice/sdl/slirp/gtk/vnc 等),双变体仅差 `--enable-latx-opt-bql` |

> 2026-08-06 更正：本页记录的是历史基线。edu 0x08 写依赖 BQL 防止
> `COMPUTING` 状态并发 0->1，不适合作为 lockless 负载。当前
> `bql-stress.c` 已改为写原子实现的 0x20、读 0x00；后续结果不可与本页
> 吞吐数值直接混为同一负载比较。

## 基线数据(5 轮)

### opt-bql ON(当前默认,spinlock BQL)

| run | MMIO ops/s | shootdown ops/s | wall(s) |
|---|---|---|---|
| 0 | 1,296,890 | 7,816,918 | 30.2 |
| 1 | 1,631,542 | 7,813,093 | 29.2 |
| 2 | 2,461,597 | 13,066,221 | 41.3 |
| 3 | 1,488,498 | 9,896,979 | 29.3 |
| 4 | 1,536,520 | 9,092,483 | 29.3 |
| **mean** | **1,683,009 (CV 26.9%)** | **9,537,139 (CV 22.7%)** | |

QSP 全部为 0:opt 构建 BQL 走裸 `pthread_spin_lock`(softmmu/cpus.c:529),绕过 QSP 插桩,符合预期。

### opt-bql OFF(mutex+futex BQL,QSP 可观测)

| run | MMIO ops/s | shootdown ops/s | QSP BQL 加锁次数 | QSP BQL 累计等待(s) |
|---|---|---|---|---|
| 0 | 215,210 | 11,319,526 | 9,351,583 | 18.02 |
| 1 | 198,576 | 10,479,840 | 8,370,370 | 17.13 |
| 2 | 201,965 | 9,970,481 | 8,808,879 | 17.28 |
| 3 | 244,833 | 12,910,184 | 10,583,105 | 21.51 |
| 4 | 207,665 | 10,578,970 | 9,037,002 | 17.11 |
| **mean** | **213,650 (CV 8.7%)** | **11,051,800 (CV 10.4%)** | **9,230,188 (CV 9.1%)** | **18.21 (CV 10.3%)** |

### 对比结论

- **现有 LATX_OPT_BQL(spinlock)在 MMIO 密集负载下吞吐是 mutex+futex 路径的 7.9 倍**(1.68M vs 214K ops/s),已有优化价值被量化证实
- 20s 负载内 BQL 加锁约 **920 万次**(约 46 万次/s),各 vCPU/iothread 累计等待 **18.2s**(4 vCPU + iothread 并行,聚合值)
- opt 构建 MMIO 线程 1→2 扩展性:1.41M/s(单线程诊断)→ 1.68M/s(2 线程),仅 1.2x,**全局锁串行化仍是瓶颈**

## BQL 调用点分解(opt-bql OFF,run-2,按等待排序)

| 调用点 | 含义 | 次数 | 等待(s) | 平均(us) |
|---|---|---|---|---|
| accel/tcg/cputlb.c:3531 | io_writex MMIO 写慢路径 `QEMU_IOTHREAD_LOCK_GUARD` | 4,085,167 | 7.90 | 1.93 |
| accel/tcg/cputlb.c:2902 | io_readx MMIO 读慢路径 `QEMU_IOTHREAD_LOCK_GUARD` | 4,058,664 | 7.77 | 1.92 |
| accel/tcg/cpu-exec.c:1385 | 中断处理取 BQL(IPI/IRQ 投递) | 154,405 | 0.90 | 5.82 |
| accel/tcg/cputlb.c:4082 | latx_apic_mem_write(APIC MMIO 写) | 80,805 | 0.27 | 3.40 |
| tcg-accel-ops-mttcg.c:134 | MTTCG 慢路径 | 71,174 | 0.22 | 3.03 |
| util/main-loop.c:311 | 主循环 | 57,378 | 0.18 | 3.07 |
| softmmu/physmem.c:2841 | physmem 路径 | 301,275 | 0.02 | 0.06 |

## 瓶颈结论

**瓶颈是进入 BQL 的次数,而不是临界区长度。**

- 约 88% 的 BQL 加锁(810 万次/920 万次)来自每次 guest MMIO 读/写慢路径的全局锁 guard(cputlb.c:2902/3531)
- 单次等待均值仅 ~1.9us,但频率(46 万次/s)× 全局串行使 vCPU 无法并行做 MMIO
- 优化方向优先级:
  1. **扩大 opt_mmio 直通面**:高频设备寄存器在翻译码内直接完成、不进 BQL(现有 APIC EOI 模式的推广)
  2. **设备级锁**:MMIO dispatch 用 per-device/MemoryRegion 锁替代全局 BQL,使不同 vCPU 访问不同设备可并行
  3. APIC MMIO 写(cputlb.c:4082)与中断路径(cpu-exec.c:1385)合计等待占比小,作为次级目标

## 测试稳定性

- 10/10 正式轮全部完成((20,2,2) 负载);曾观测 (30,2,2) 偶发不结束,未复现于标准负载
- 同机同配置 CV:opt 22-27%,no-opt 9-10%;后续对比测量须同机空闲时进行,轮数 ≥5
- 局限:perf 在本平台对 attach 进程采样受限,争用度量以 QSP + 客户机吞吐为准;opt 构建 BQL 无 QSP 插桩,以吞吐和扩展性间接表征

## 原始数据

3a6000:~/bql-lats/results/{baseline-opt,baseline-noopt}/(含每轮 guest.log、qsp.txt、result.json、summary.json)
