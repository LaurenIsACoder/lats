# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

i386_ss.add(files(
  'file_ctx.c',
  'sys-aot.c',
  'sys-aot-common.c',
  'sys-aot-format.c',
  'sys-aot-save.c',
  'sys-aot-check.c',
  'sys-aot-load.c',
  'sys-aot-reloc.c',
  'sys-aot-merger.c',
  'merge.c',
  'sys-aot-tu-codec.c',
  'sys-aot-tu-merge.c',
 ))
i386_ss.add(when: 'CONFIG_SOFTMMU', if_true: files(
  'sys-aot-offline.c',
  'sys-aot-runtime-info.c',
  'sys-aot-atomic.c',
  'sys-aot-cpu-query.c',
  'sys-aot-system.c',
  'sys-aot-x87.c',
))
