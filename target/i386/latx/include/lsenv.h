/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LSENV_H_
#define _LSENV_H_

#include "env.h"
#include "qemu-def.h"
#include "reg-alloc.h"
#include "translate.h"

extern __thread ENV *lsenv;

#ifdef CONFIG_LATX_TU
bool latxs_gpr_highbits_filter(IR2_INST *ir2);
#endif

/* func to access ENV's attributes */
static inline int lsenv_offset_of_save_regs(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->save_regs[i]) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_gpr(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->regs[i]) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_eflags(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->eflags) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_tb_jmp_cache_ptr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->tb_jmp_cache_ptr) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_latx_njc_cache_ptr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->latx_njc_cache_ptr) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fcsr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fcsr) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_old_mxcsr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->old_mxcsr) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_eip(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->eip) - (ADDR)lsenv->cpu_state);
}
static inline int lsenv_offset_of_neip(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->neip) - (ADDR)lsenv->cpu_state);
}
static inline int lsenv_offset_of_sp(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->host_sp) - (ADDR)lsenv->cpu_state);
}
static inline int lsenv_offset_of_top(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpstt) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fp_status(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fp_status) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_ft0(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->ft0) - (ADDR)lsenv->cpu_state);
}

#ifdef CONFIG_LATX_OPT_NOFP
static inline int lsenv_offset_of_nofp(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->nofp) - (ADDR)cpu);
}
#else
static inline int lsenv_offset_of_mode_fpu(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->mode_fpu) - (ADDR)lsenv->cpu_state);
}
#endif

static inline int lsenv_offset_of_sse_status(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->sse_status) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_cpu_index(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    CPUState *cpu_state = env_cpu(cpu);
    return (int)((ADDR)(&cpu_state->cpu_index) - (ADDR)lsenv->cpu_state);
}

/* FPU top */
static inline int lsenv_get_top(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return cpu->fpstt;
}

static inline void lsenv_set_top(ENV *lsenv, int new_fpstt)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    cpu->fpstt = new_fpstt;
}

static inline FPReg lsenv_get_fpregs(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return cpu->fpregs[i];
}

static inline void lsenv_set_fpregs(ENV *lsenv, int i, FPReg new_value)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    cpu->fpregs[i] = new_value;
}

static inline int lsenv_offset_of_status_word(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpus) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_control_word(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpuc) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fpcs(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpcs) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fpds(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpds) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fpip(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpip) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fpdp(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpdp) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_get_fpu_control_word(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return cpu->fpuc;
}

static inline int lsenv_offset_of_tag_word(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fptags[0]) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_fpr(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR) & (cpu->fpregs[i].d) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_mmx(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->fpregs[i]) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_mxcsr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->mxcsr) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_excp_divz(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->excp_divz) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_seg_base(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->segs[i].base) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_seg_selector(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->segs[i].selector) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_seg_limit(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->segs[i].limit) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_seg_flags(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->segs[i].flags) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_ldt_base(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->ldt.base) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_ldt_limit(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->ldt.limit) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_gdt_base(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->gdt.base) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_gdt_limit(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->gdt.limit) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_new_cs(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->new_cs) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_cs_e1(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->cs_e1) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_cs_e2(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->cs_e2) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_old_cpl(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->old_cpl) - (ADDR)lsenv->cpu_state);
}

/* func to access ENV's interrupt/exception attributes */
static inline int lsenv_offset_exception_index(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    CPUState *cpu_state = env_cpu(cpu);
    return (int)((ADDR)(&cpu_state->exception_index) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_exception_next_eip(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->exception_next_eip) - (ADDR)lsenv->cpu_state);
}

/* virtual registers */
static inline int lsenv_offset_of_vreg(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->vregs[i]) - (ADDR)lsenv->cpu_state);
}

static inline int lsenv_offset_of_guest_base(ENV *lsenv)
{
    return lsenv_offset_of_vreg(lsenv, 0);
}

static inline int lsenv_offset_of_last_executed_tb(ENV *lsenv)
{
    return lsenv_offset_of_vreg(lsenv, 1);
}

static inline int lsenv_offset_of_next_eip(ENV *lsenv)
{
    return lsenv_offset_of_vreg(lsenv, 2);
}

static inline int lsenv_offset_of_top_bias(ENV *lsenv)
{
    return lsenv_offset_of_vreg(lsenv, 3);
}

static inline int lsenv_offset_of_ss(ENV *lsenv)
{
    return lsenv_offset_of_vreg(lsenv, 4);
}

static inline int lsenv_offset_of_xmm(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->xmm_regs[i]) - (ADDR)lsenv->cpu_state);
}

static inline ADDR lsenv_get_vreg(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (ADDR)cpu->vregs[i];
}

static inline void lsenv_set_vreg(ENV *lsenv, int i, ADDR val)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    cpu->vregs[i] = (uint64_t)val;
}

static inline ADDR lsenv_get_guest_base(ENV *lsenv)
{
    return (ADDR)lsenv_get_vreg(lsenv, 0);
}

static inline void lsenv_set_guest_base(ENV *lsenv, ADDR gbase)
{
    lsenv_set_vreg(lsenv, 0, gbase);
}

static inline ADDR lsenv_get_last_executed_tb(ENV *lsenv)
{
    return (ADDR)lsenv_get_vreg(lsenv, 1);
}

static inline void lsenv_set_last_executed_tb(ENV *lsenv, ADDR tb)
{
    lsenv_set_vreg(lsenv, 1, tb);
}

static inline ADDRX lsenv_get_next_eip(ENV *lsenv)
{
    return (ADDRX)lsenv_get_vreg(lsenv, 2);
}

static inline void lsenv_set_next_eip(ENV *lsenv, ADDRX eip)
{
    lsenv_set_vreg(lsenv, 2, eip);
}

static inline int lsenv_get_top_bias(ENV *lsenv)
{
    return (int)lsenv_get_vreg(lsenv, 3);
}

static inline void lsenv_set_top_bias(ENV *lsenv, int top_bias)
{
    lsenv_set_vreg(lsenv, 3, top_bias);
}

static inline int lsenv_offset_of_tr_data(ENV *lsenv)
{
    return (ADDR)(&lsenv->tr_data) - (ADDR)lsenv;
}

static inline int lsenv_offset_of_cc_src(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->cc_src) - (ADDR)env);
}

static inline int lsenv_offset_of_cc_op(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->cc_op) - (ADDR)env);
}

static inline int lsenv_offset_of_df(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->df) - (ADDR)env);
}

static inline int lsenv_offset_of_fmask(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->fmask) - (ADDR)env);
}

static inline int lsenv_offset_of_hflags(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->hflags) - (ADDR)env);
}

static inline int lsenv_offset_of_hflags2(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->hflags2) - (ADDR)env);
}

static inline int lsenv_offset_of_star(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->star) - (ADDR)env);
}

static inline int lsenv_offset_of_lstar(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->lstar) - (ADDR)env);
}
static inline int lsenv_offset_of_cstar(ENV *lsenv)
{
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&env->cstar) - (ADDR)env);
}

static inline int lsenv_offset_of_idtr_base(ENV *lsenv)
{
    return offsetof(CPUX86State, idt.base);
}

static inline int lsenv_offset_of_idtr_limit(ENV *lsenv)
{
    return offsetof(CPUX86State, idt.limit);
}

static inline int lsenv_offset_of_gdtr_base(ENV *lsenv)
{
    return offsetof(CPUX86State, gdt.base);
}

static inline int lsenv_offset_of_gdtr_limit(ENV *lsenv)
{
    return offsetof(CPUX86State, gdt.limit);
}

static inline int lsenv_offset_of_ldtr_selector(ENV *lsenv)
{
    return offsetof(CPUX86State, ldt.selector);
}

static inline int lsenv_offset_of_tr_selector(ENV *lsenv)
{
    return offsetof(CPUX86State, tr.selector);
}

static inline int lsenv_offset_of_cr(ENV *lsenv, int i)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->cr[i]) - (ADDR)cpu);
}

static inline int lsenv_offset_of_efer(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->efer) - (ADDR)cpu);
}

static inline int lsenv_offset_of_tpr(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->tpr) - (ADDR)cpu);
}

static inline int lsenv_offset_of_isrv(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->isrv) - (ADDR)cpu);
}

static inline int lsenv_offset_of_irrv(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->irrv) - (ADDR)cpu);
}

static inline int lsenv_offset_of_syscall_tb(ENV *lsenv)
{
    CPUX86State *cpu = (CPUX86State *)lsenv->cpu_state;
    return (int)((ADDR)(&cpu->syscall_tb) - (ADDR)cpu);
}

inline __attribute__ ((always_inline))
IR2_INST *ir2_get(int id)
{
    lsassert(id >= 0 && id < lsenv->tr_data->ir2_cur_nr);
    return lsenv->tr_data->ir2_array + id;
}

inline __attribute__ ((always_inline))
void ir2_append(IR2_INST *ir2)
{
#ifdef CONFIG_LATX_TU
    if ((option_aot_gpr_highbits || option_tu_gpr_highbits) &&
        latxs_gpr_highbits_filter(ir2)) {
        return;
    }
#endif
    if (ir2_opcode(ir2) >= LISA_GR2SCR ||
        ir2_opcode(ir2) == LISA_DUP) {
        lsenv->tr_data->ir2_asm_nr++;
    }
}

inline __attribute__ ((always_inline))
IR2_OPND latxs_ir2_opnd_new_label(void)
{
    IR2_OPND opnd;
    opnd._type = IR2_OPND_LABEL;
    opnd.val   = ++(lsenv->tr_data->label_num);
    return opnd;
}

inline __attribute__ ((always_inline))
IR2_INST *ir2_allocate(void)
{
    TRANSLATION_DATA *t = lsenv->tr_data;

    /* 1. make sure we have enough space */
    if (t->ir2_cur_nr == t->ir2_max_nr) {
        int bytes = sizeof(IR2_INST) * t->ir2_max_nr;
        /* 1.2. double the array */
        t->ir2_max_nr *= 2;
        t->ir2_array = (IR2_INST *)mm_realloc(t->ir2_array, bytes << 1);
    }

    /* 2. allocate one */
    IR2_INST *p = t->ir2_array + t->ir2_cur_nr;
    ir2_set_id(p, t->ir2_cur_nr);
    t->ir2_cur_nr++;

    p->_b_label_id = 0;
    p->_b_ir2_id = 0;
    p->_b_label_name = NULL;
#if defined(CONFIG_LATX_IR2_COMMENT)
    p->comment = NULL;
#endif

    /* 3. return it */
    return p;
}

static inline __attribute__ ((always_inline))
int latxs_ir2_opnd_is_itemp(IR2_OPND *opnd)
{
    return latxs_ir2_opnd_is_gpr(opnd) && latxs_ir2_opnd_is_reg_temp(opnd);
}

static inline __attribute__ ((always_inline))
int latxs_ir2_opnd_is_ftemp(IR2_OPND *opnd)
{
    return latxs_ir2_opnd_is_fpr(opnd) && latxs_ir2_opnd_is_reg_temp(opnd);
}

inline __attribute__ ((always_inline))
uint64_t get_current_inst_addr(TranslationBlock *tb)
{
    return (uint64_t)(tb->tc.ptr + (lsenv->tr_data->ir2_asm_nr << 2));
}

static inline void latxs_tr_gen_hflags_set(uint32_t mask)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (td->sys.flags & mask) {
        return;
    }

#ifdef CONFIG_LATX_DEBUG
    TranslationBlock *tb = td->curr_tb;
    tb->inhibit_irq_type = 1;
#endif

    IR2_OPND mask_reg   = latxs_ra_alloc_itemp();
    IR2_OPND hflags_reg = latxs_ra_alloc_itemp();

    latxs_load_imm32_to_ir2(&mask_reg, mask, EXMode_N);

    latxs_append_ir2_opnd2i(LISA_LD_W, &hflags_reg, &latxs_env_ir2_opnd,
            lsenv_offset_of_hflags(lsenv));
    latxs_append_ir2_opnd3(LISA_OR, &hflags_reg, &hflags_reg, &mask_reg);
    latxs_append_ir2_opnd2i(LISA_ST_W, &hflags_reg, &latxs_env_ir2_opnd,
            lsenv_offset_of_hflags(lsenv));

    latxs_ra_free_temp(&mask_reg);
    latxs_ra_free_temp(&hflags_reg);
}

static inline void latxs_tr_gen_hflags_clr(uint32_t mask)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (!(td->sys.flags & mask)) {
        return;
    }

#ifdef CONFIG_LATX_DEBUG
    TranslationBlock *tb = td->curr_tb;
    tb->inhibit_irq_type = 2;
#endif

    IR2_OPND mask_reg   = latxs_ra_alloc_itemp();
    IR2_OPND hflags_reg = latxs_ra_alloc_itemp();

    latxs_load_imm32_to_ir2(&mask_reg, ~mask, EXMode_N);

    latxs_append_ir2_opnd2i(LISA_LD_W, &hflags_reg, &latxs_env_ir2_opnd,
            lsenv_offset_of_hflags(lsenv));
    latxs_append_ir2_opnd3(LISA_AND, &hflags_reg, &hflags_reg, &mask_reg);
    latxs_append_ir2_opnd2i(LISA_ST_W, &hflags_reg, &latxs_env_ir2_opnd,
            lsenv_offset_of_hflags(lsenv));

    latxs_ra_free_temp(&mask_reg);
    latxs_ra_free_temp(&hflags_reg);
}

#endif
