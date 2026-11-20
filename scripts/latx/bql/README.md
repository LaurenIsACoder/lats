# BQL 竞争基准测试设施

测量 LATS 系统模式下 BQL(Big QEMU Lock)竞争的可复现基准。

## 组成

| 文件 | 作用 |
|------|------|
| `bql-stress.c` | 客户机负载:edu PCI 设备 BAR0 原子状态写(0x20)+常量读(0x00),以及 mprotect TLB shootdown IPI 风暴 |
| `edu-lockless-test.c` | edu 逐寄存器 lockless 专项:对比安全的 0x00/0x20 与仍需 BQL 的 0x04 |
| `linux-drm-stress.c` | Linux 客户机直接 DRM/KMS 全屏刷新负载,用于 virtio-gpu 的 BQL 设备归因 |
| `prepare-initrd.sh` | 在 buildroot rootfs.cpio 基础上注入负载与 rdinit,产出 prepared initrd |
| `run-bql-bench.py` | 宿主驱动:启动 lats、等待 DONE 标记、经 monitor 取 `info sync-profile`(QSP)、汇总多轮结果 |

## 测量原理

- **端到端吞吐**:客户机侧 MMIO ops/s、shootdown ops/s(负载打印 `[BQL-STRESS]` 行)
- **QSP 调用点分析**:`-enable-sync-profile` + HMP `info sync-profile`,给出 BQL 各调用点的加锁次数/累计等待/平均等待
- `CONFIG_LATX_OPT_BQL` 的 spinlock 已接入 QSP；仍可用双变体区分锁实现本身的影响:
  - `build/`(opt-bql ON):端到端吞吐基线 + spinlock QSP 数据
  - `build-nooptbql/`(opt-bql OFF):QSP 调用点分解 + 对照吞吐

## 使用(3A6000)

```bash
# 1. x86 主机上编译负载并同步
gcc -O2 -static -pthread -o bql-stress bql-stress.c
rsync -a bql-stress 3a6000:$LAT_DIR/scripts/latx/bql/

# 2. 准备 initrd(mknod 需要 root;参数: 时长 mmio线程数 sd线程数)
sudo ./prepare-initrd.sh \
    /absolute/x86-rootfs/buildroot/output/images/rootfs.cpio \
    bql-stress ../bql-c22.cpio 20 2 2

# 3. 关闭 SFB(HAMT 需要)
sudo sh -c 'echo 0 > /sys/kernel/debug/loongarch/sfb_state'

# 4. 运行基准(5 轮,-smp 4)
python3 run-bql-bench.py \
    --qemu $LAT_DIR/build/lats-x86_64 \
    --kernel /absolute/x86-rootfs/buildroot/output/images/bzImage \
    --initrd $LAT_DIR/bql-c22.cpio \
    --smp 4 --runs 5 --duration-margin 400 \
    --out-dir results/baseline-opt --label "opt-bql ON"
```

每轮产出 `run-N/{guest.log,qsp.txt,result.json,qemu-cmd.json}`,汇总为 `summary.json`。

## Linux virtio-gpu 图形负载

`linux-drm-stress.c` 不依赖 Xorg 或 libdrm 运行库。它用 DRM ioctl 创建
dumb buffer,切换到选定的 KMS 显示模式,并连续提交全屏更新。编译时只需要
libdrm 的 UAPI 头文件:

```bash
gcc -O2 -Wall -Wextra -static -I/usr/include/libdrm \
    -o linux-drm-stress linux-drm-stress.c

# 客户机内运行;访问 card0 通常需要 root 或 video 组权限
sudo ./linux-drm-stress /dev/dri/card0 30
```

负载覆盖 Linux 2D 扫描输出、virtio-gpu control queue 通知和显示更新。
它不创建 virgl/venus 3D 上下文,不能替代带宿主 GPU 的 OpenGL/Vulkan 应用测试。

## 注意

- 基准对宿主负载敏感:测量期间保持机器空闲,否则 CV 显著增大(观测 9-27%)
- 曾观测 (30,2,2) 负载偶发不结束;当前标准负载 (20,2,2) 在 5+5 轮中全部完成
- perf 在本平台对 attach 进程采样受限,争用指标以 QSP 为准
- QSP 会增加热路径开销,仅在诊断时开启;性能结果必须同时保留 QSP 关闭对照
