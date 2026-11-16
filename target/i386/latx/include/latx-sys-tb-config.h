/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TB_CONFIG_H_
#define _LATX_SYS_TB_CONFIG_H_

extern int option_tb_config;

#define LATXS_TBCFG_USERMODE    0x2
#define LATXS_TBCFG_EXITCODE    0x1

static inline int latxs_tb_config_exitcode(void)
{
    return option_tb_config & LATXS_TBCFG_EXITCODE;
}

static inline int latxs_tb_config_usermode(void)
{
    return option_tb_config & LATXS_TBCFG_USERMODE;
}

#endif
