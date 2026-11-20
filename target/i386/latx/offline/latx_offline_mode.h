/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * latx_offline_mode.h - Offline X86 to LoongArch translation mode header
 */

#ifndef LATX_OFFLINE_MODE_H
#define LATX_OFFLINE_MODE_H

#include "qemu/osdep.h"

/* Global flag to indicate offline mode */
extern int latx_offline_mode_active;

/* Check if running in offline mode, returns index of -offline arg or 0 */
int check_offline_mode(int argc, char **argv);

/* Run offline translation mode */
int run_offline_mode(int argc, char **argv, int offline_idx);

/* Main offline translation function */
int latx_offline_translate(const char *hex_bytes, int mode, uint64_t pc, int max_insns);

#endif /* LATX_OFFLINE_MODE_H */
