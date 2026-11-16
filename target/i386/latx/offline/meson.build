# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

# Meson build for offline X86 disassembly mode

offline_mode_src = files(
  'latx_offline_mode.c'
)

# Add to i386_ss so it gets compiled into the QEMU binary
i386_ss.add(offline_mode_src)
