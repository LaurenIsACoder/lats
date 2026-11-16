/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_GPR_HIGHBITS_POLICY_H
#define LATX_GPR_HIGHBITS_POLICY_H

#include <stdbool.h>

/* AOT cache misses are normal dynamic translations. */
static inline bool latxs_gpr_hb_dynamic_tu_is_active(
    bool in_aot_translation, bool tu_enabled, bool option_enabled)
{
    return !in_aot_translation && tu_enabled && option_enabled;
}

#endif
