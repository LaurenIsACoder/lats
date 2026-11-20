/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_RAS_H
#define LATX_RAS_H
#include "loongarch-extcontext.h"

#include "latx/include/ir1.h"
#include "latx/include/ir2.h"
#include "latx/include/latx-ras-config.h"

struct RAS_Entry {
    uint64_t pc;
    const void *ptr;    /* pointer to the translated code */
};

struct __attribute ((packed)) Soft_RAS {
    int reserved;
    int top;
    TranslationBlock* patching_tb;
    RAS_Entry stack[RAS_SIZE];
};

#ifdef CONFIG_RAS_PROFILER
struct RAS_Profile {
    long long total;
    long long hit;
    long long period_total;
    long long period_hit;
    uint64_t cur_time;
};
#endif

#define UC_PC(uc) ((uc)->uc_mcontext.__pc)
#define UC_GR(uc) ((uc)->uc_mcontext.__gregs)
#ifndef LOONGARCH_NEW_WORLD
#define UC_FREG(uc) ((uc)->uc_mcontext.__fpregs)
#endif

bool latxs_ras_link(TranslationBlock *tb, TranslationBlock *tb_next);
bool latxs_ras_close_relinkable(TranslationBlock *tb);
void latxs_ras_unlink(TranslationBlock *tb);
void latxs_ras_unlink_target_locked(TranslationBlock *tb);
void latxs_init_rr_thread_sigsegv(CPUState *cpu);
uint64_t ras_target_check(CPUX86State *env, uint64_t gva, uint64_t hva);
TranslationBlock *export_tb_lookup(CPUState *cpu, vaddr pc,
                uint64_t cs_base, uint32_t flags, uint32_t cflags);
void latxs_flush_soft_ras(CPUState *cpu);
bool latxs_translate_ras_push(IR1_INST *pir1);
bool latxs_translate_ras_pop(IR2_OPND  *ret_gva);
void ras_debug_helper(CPUX86State *env, TranslationBlock *tb);
uint32_t get_ras_cpuindex(TranslationBlock *tb);
void set_ras_cpuindex(TranslationBlock *tb, uint32_t index);
void latxs_ras_init(CPUState *cpu);
void ras_total_count(CPUX86State *env);
void ras_hit_count(CPUX86State *env);
void ras_profiler(CPUState *cpu);

#endif /* LATX_RAS_H */
