/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LATX_GHBR_H
#define LATX_GHBR_H

#include "ir1.h"

#ifdef CONFIG_LATX_TU
void latxs_ghbr_analyze_tu(uint32_t tb_num, TranslationBlock **tb_list);
bool latxs_ghbr_can_elide_write(const IR1_INST *ir1, unsigned int reg);
uint64_t latxs_ghbr_stats_checkpoint(void);
void latxs_ghbr_stats_restore(uint64_t checkpoint);
void latxs_ghbr_stats_commit(void);
#else
static inline bool latxs_ghbr_can_elide_write(const IR1_INST *ir1,
                                              unsigned int reg)
{
    return false;
}
static inline uint64_t latxs_ghbr_stats_checkpoint(void)
{
    return 0;
}
static inline void latxs_ghbr_stats_restore(uint64_t checkpoint)
{
    (void)checkpoint;
}
static inline void latxs_ghbr_stats_commit(void)
{
}
#endif

static inline bool latxs_ghbr_can_omit(const IR1_INST *ir1, unsigned int reg)
{
#ifdef CONFIG_LATX_TU
    return ir1 && reg < 16 && (ir1->ghbr_opt & (1U << reg));
#else
    return false;
#endif
}

#endif
