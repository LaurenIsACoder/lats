/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_ENV_INIT_FUNC_H_
#define _LATX_SYS_ENV_INIT_FUNC_H_

void latx_lsenv_init(CPUArchState *env);
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
void latxs_aot_close_open_linked_calls(CPUArchState *env);
#endif

#endif
