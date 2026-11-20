# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

i386_ss.add(files(
  'flag-lbt.c',
  'sys-lsfpu.c',
  'sys-staticcs.c',
  'sys-flag-rdtn.c',
  'sys-tu.c',
  'sys-tu-page-lock.c',
  'sys-intb.c',
  'sys-intb-njc.c',
  'sys-intb-fsht.c',
  'sys-cross-page.c',
  'sys-inst-ptn.c',
  'sys-hamt-dmw.c',
  'sys-ras.c',
  'sys-sigfpe.c',
  'sys-push-pop.c',
  'sys-imm-cache.c'
))
i386_ss.add(when: 'CONFIG_LATX_TU', if_true: files(
  'sys-ghbr.c',
  'sys-shbr.c',
))
i386_ss.add(when: 'CONFIG_SOFTMMU', if_true: files(
  'sys-hamt-excp.c',
  'sys-mmio.c',
  'sys-sigrt.c',
))
