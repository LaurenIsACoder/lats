================================================
LATS (LoongArch Architecture Translator System)
================================================

.. image:: https://img.shields.io/badge/license-GPL--2.0-blue.svg
   :target: COPYING
   :alt: GPL-2.0 license

LATS（LoongArch Architecture Translator System）即龙架构系统级架构转译器，
是一个面向 LoongArch 架构的高性能系统级二进制翻译器，可在龙架构平台上
运行 64 位 x86 操作系统，并进一步支持 32 位和 64 位 x86 应用程序。

LATS 基于 QEMU 8.1.5 release 开发并进行深度优化，性能相较原生 QEMU
有显著提升。QEMU 8.1.5 对应的上游原始提交为
``20a1b341a0af1fef84cec9e521d33da0e8d9ecf3``，该提交节点与上游 QEMU
项目一致。系统级模拟通常从处理器、内存和设备三个方面衡量：处理器模拟通过
二进制翻译实现跨架构指令执行，内存模拟基于 QEMU SoftMMU 扩展和优化，设备
模拟复用 QEMU 的硬件模型，并通过 virtio 和 IOMMU 支持设备直通。

`See the English Version Here >> <README.en.rst>`_


项目背景
========

在 LoongArch 生态建设中，用户级二进制翻译器 `LAT
<https://github.com/lat-opensource/lat>`_ 已支持大量 x86 应用在龙架构平台
运行，但用户级兼容方案需要逐一处理应用运行时的差异。系统级翻译可以在虚拟
机边界内提供更完整的兼容环境，同时也要承担更高的性能开销。LATS 通过针对性
优化降低这部分开销，目前可在 3A6000 平台上流畅运行 Windows 7。


项目结构
========

.. code-block:: text

   lats
   ├── latxbuild/                            # 编译脚本
   ├── hamt/hamt-pm/                         # ABI1.0 HAMT 宿主内核模块
   ├── accel/tcg/                            # QEMU TCG 框架
   ├── tcg/
   │   └── region.c                          # 代码缓存空间
   ├── target/i386/
   │   ├── hamt/                             # HAMT 相关实现
   │   └── latx/                             # 二进制翻译引擎
   │       ├── ir1/                          # IR1：x86 指令 IR 表示
   │       ├── ir2/                          # IR2：LoongArch 指令 IR 表示
   │       ├── optimization/                 # EFLAGS、HAMT、RAS 等优化
   │       ├── sbt/                          # AOT 相关
   │       ├── translator/                   # 翻译与寄存器分配
   │       ├── include/                      # LATX 内部头文件
   │       └── capStone/                     # 反汇编器
   ├── softmmu/                              # 系统模式入口和内存管理
   ├── hw/                                   # 硬件设备模拟
   └── README.rst                            # 本文档


环境要求
========

- 构建和运行环境为 LoongArch Linux，目标客户机为 64 位 x86。
- 完整优化路径需要宿主 CPU 和内核提供 LSX、LASX 与 LBT_X86 扩展。
- HAMT 需要宿主内核支持。启用 HAMT 时需按平台说明关闭 SFB（Store Fill
  Buffer）。
- 构建需要 Python 3、Meson、Ninja、GCC/G++、GLib、Pixman、zlib 以及
  SPICE、SDL2、slirp、libusb 开发包。实际依赖取决于配置选项。


编译
====

克隆源码并初始化子模块：

.. code-block:: bash

   git clone --depth=1 --recursive https://github.com/lat-opensource/lats.git
   cd lats

Debian / Ubuntu 可安装以下构建依赖：

.. code-block:: bash

   sudo apt install -y git make gcc g++ pkg-config python3 python3-venv \
       python3-pip python3-setuptools meson ninja-build libglib2.0-dev \
       libpixman-1-dev libspice-protocol-dev libspice-server-dev \
       libsdl2-dev libslirp-dev libusb-1.0-0-dev zlib1g-dev

Fedora 可安装：

.. code-block:: bash

   sudo dnf install -y git make gcc gcc-c++ pkgconf-pkg-config python3 \
       python3-pip python3-setuptools meson ninja-build glib2-devel \
       pixman-devel spice-protocol spice-server-devel SDL2-devel \
       libslirp-devel libusb1-devel zlib-devel

默认构建：

.. code-block:: bash

   ./latxbuild/build.sh -c def

生成的系统级翻译器位于 ``build/lats-x86_64``。发布构建可使用
``./latxbuild/build.sh -c rel``，调试构建可使用
``./latxbuild/build.sh -c dbg``。其他配置通过 ``-h`` 查看。


运行
====

使用 HAMT 前先关闭宿主 SFB：

.. code-block:: bash

   sudo sh -c 'echo 0 > /sys/kernel/debug/loongarch/sfb_state'

然后启动 x86 客户机：

.. code-block:: bash

   ./build/lats-x86_64 -hda /path/to/disk.img -m 8192 -smp 4

若宿主内核不支持 HAMT，可显式关闭：

.. code-block:: bash

   ./build/lats-x86_64 -hda /path/to/disk.img -m 8192 -smp 4 \
       -latx select,hamt=0


文档与反馈
==========

- `问题反馈 <https://github.com/lat-opensource/lats/issues>`_
- `贡献指南 <CONTRIBUTING.md>`_
- `贡献者名单 <CONTRIBUTORS.md>`_
- `编译和运行 <docs/build-and-run.md>`_


许可证
======

本项目基于 QEMU 8.1.5 源代码进行二次开发。QEMU 整体以 GNU 通用公共许可证
第 2 版（GNU General Public License version 2，GPLv2）发布；项目中保留的
第三方代码继续遵循各自文件中的许可证。详见 `COPYING <COPYING>`_、
`LICENSE <LICENSE>`_ 和 `NOTICE <NOTICE>`_。


致谢
====

特别鸣谢 QEMU 项目及其贡献者。QEMU 为本项目提供了系统模拟基础。

------------

如有问题或建议，欢迎通过 `Issue
<https://github.com/lat-opensource/lats/issues>`_ 与我们交流。
