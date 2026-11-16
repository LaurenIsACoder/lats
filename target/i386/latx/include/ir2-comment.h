/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_IR2_COMMENT_H_
#define _LATX_IR2_COMMENT_H_

#if defined(CONFIG_LATX_IR2_COMMENT)

#define la_comment_ir2(p, n)    latxs_ir2_add_comment(p, n)
#define la_comment(n)           latxs_ir2_add_comment_on_curr(n)

void latxs_ir2_add_comment(IR2_INST *pir2, const char *n);
void latxs_ir2_add_comment_on_curr(const char *n);

#else

#define la_comment_ir2(p, n)
#define la_comment(n)

#endif

#endif
