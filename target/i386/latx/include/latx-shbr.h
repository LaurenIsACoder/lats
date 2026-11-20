/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_SHBR_H
#define LATX_SHBR_H

#include "exec/exec-all.h"
#include "ir1.h"

#define LATXS_SHBR_HIGH32 (1U << 0)
#define LATXS_SHBR_HIGH64 (1U << 1)
#define LATXS_SHBR_RECOVER_ZERO64 (1U << 2)

void latxs_tr_shbr_restore_zero64(uint16_t mask);

#ifdef CONFIG_LATX_TU
#define LATXS_SHBR_ON_32(ir1) \
    (((ir1)->shbr_flags & LATXS_SHBR_HIGH32) != 0)
#define LATXS_SHBR_ON_64(ir1) \
    (((ir1)->shbr_flags & LATXS_SHBR_HIGH64) != 0)

void latxs_shbr_analyze_tu(uint32_t tb_num, TranslationBlock **tb_list,
                           uint32_t cpuid_ext_features);
#else
#define LATXS_SHBR_ON_32(ir1) false
#define LATXS_SHBR_ON_64(ir1) false
#endif

static inline uint16_t latxs_shbr_live_before(uint16_t live_after,
        uint16_t read, uint16_t def, uint16_t dep)
{
    if (live_after & def) {
        live_after = (live_after & ~def) | dep;
    }
    return live_after | read;
}

#endif
