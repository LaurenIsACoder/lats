/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SIGINT_SYS_H_
#define _LATX_SIGINT_SYS_H_

void latxs_init_rr_thread_sigfpe(CPUState *cpu);

/*
 * This helper reads LATX translated-code register mappings, so it belongs to
 * LATX user-mode translation and not to generic TCG user-mode execution.
 */
#if defined(CONFIG_LATX_OPT) && !defined(CONFIG_LATX_TCG_MODE)
void latxs_save_sync_signal_state(CPUState *cpu, void *context);
#endif
#endif
