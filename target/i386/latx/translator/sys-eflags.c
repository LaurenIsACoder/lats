/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "sys-excp.h"

void latxs_sys_eflags_register_ir1(void)
{
    latxs_register_ir1(X86_INS_CLD);
    latxs_register_ir1(X86_INS_STD);
    latxs_register_ir1(X86_INS_CLC);
    latxs_register_ir1(X86_INS_STC);
    latxs_register_ir1(X86_INS_CMC);
    latxs_register_ir1(X86_INS_CLAC);
    latxs_register_ir1(X86_INS_STAC);
    latxs_register_ir1(X86_INS_PUSHF);
    latxs_register_ir1(X86_INS_PUSHFD);
    latxs_register_ir1(X86_INS_POPF);
    latxs_register_ir1(X86_INS_POPFD);
    latxs_register_ir1(X86_INS_PUSHFQ);
    latxs_register_ir1(X86_INS_POPFQ);

    latxs_register_ir1(X86_INS_CLTS);

    latxs_register_ir1(X86_INS_SAHF);
    latxs_register_ir1(X86_INS_LAHF);
}

/*
 * always set CPUX86State.cc_src == eflags
 *
 * Only called in :
 * 1. context switch native to bt
 * 2. call to helper prologue
 */
void latxs_tr_save_eflags(void)
{
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *env = &latxs_env_ir2_opnd;

    /* 1. store OF/SF/ZF/AF/PF/CF in env->cc_src */
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &temp, 0x3f);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp, env,
            lsenv_offset_of_cc_src(lsenv));

    /* 2. set env->cc_op = 1 */
    latxs_append_ir2_opnd2i(LISA_ORI, &temp, zero, 1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp, env,
            lsenv_offset_of_cc_op(lsenv));
    latxs_ra_free_temp(&temp);
}

/*
 * Only called in :
 * 1. context switch bt to native
 * 2. call to helper epilogue
 */
void latxs_tr_load_eflags(void)
{
    /* Load cc_src(OF/SF/ZF/AF/PF/CF) into temp register */
    IR2_OPND cc_src = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_W, &cc_src, &latxs_env_ir2_opnd,
            lsenv_offset_of_cc_src(lsenv));

    /* Move OF/SF/ZF/AF/PF/CF into CPU */
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cc_src, 0x3f);
    latxs_ra_free_temp(&cc_src);
}

/* cpu_load_eflags from target/i386/tcg/misc_helper.c*/
void latx_cpu_load_eflags(IR2_OPND *eflags, IR2_OPND *update_mask)
{
    IR2_OPND cc_src = latxs_arg0_ir2_opnd;
    IR2_OPND tmp = latxs_arg1_ir2_opnd;

    /* env->cc_src = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C) */
    //la_out2i(LISA_LD_D, &cc_src, &latxs_env_ir2_opnd,
    //        lsenv_offset_of_cc_src(lsenv));
    la_out2i(LISA_ANDI, &cc_src, eflags,
            CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    la_out2i(LISA_ST_D, &cc_src, &latxs_env_ir2_opnd,
            lsenv_offset_of_cc_src(lsenv));

    /* env->cc_op = CC_OP_EFLAGS */
    latxs_load_imm64_to_ir2(&tmp, CC_OP_EFLAGS);
    la_out2i(LISA_ST_D, &tmp, &latxs_env_ir2_opnd,
            lsenv_offset_of_cc_op(lsenv));

//#ifdef CONFIG_LATX_OPT_EFLAGS
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cc_src, 0x3f);
//#endif

    /* env->df = 1 - (2 * ((eflags >> 10) & 1)); */
    la_out2i(LISA_SRLI_D, &cc_src, eflags, 10);
    la_out2i(LISA_ANDI, &cc_src, &cc_src, 1);

    la_out2i(LISA_SLLI_D, &cc_src, &cc_src, 1);

    latxs_load_imm64_to_ir2(&tmp, 1);
    la_out3(LISA_SUB_D, &cc_src, &tmp, &cc_src);

    la_out2i(LISA_ST_W, &cc_src, &latxs_env_ir2_opnd,
            lsenv_offset_of_df(lsenv));

    /*
     *  env->eflags = (env->eflags & ~update_mask) |
     *  (eflags & update_mask) | 0x2 
     */
    latxs_append_ir2_opnd2i(LISA_LD_WU, &cc_src, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    la_out3(LISA_ANDN, &cc_src, &cc_src, update_mask);
    latxs_append_ir2_opnd2i(LISA_ORI, &cc_src, &cc_src, 0x2);

    la_out3(LISA_AND, eflags, eflags, update_mask);
    la_out3(LISA_OR, &cc_src, &cc_src, eflags);

    la_out2i(LISA_ST_W, &cc_src, &latxs_env_ir2_opnd,
           lsenv_offset_of_eflags(lsenv));

    latxs_ra_free_temp(&cc_src);
    latxs_ra_free_temp(&tmp);
}

/*
 * Only called in :
 * 1. static context switch prologue
 */
void latxs_tr_gen_static_save_eflags(void)
{
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *env = &latxs_env_ir2_opnd;

    /* 1. store OF/SF/ZF/AF/PF/CF in env->cc_src */
    //IR2_OPND temp = latxs_stmp1_ir2_opnd;
    IR2_OPND temp = latxs_arg2_ir2_opnd;
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &temp, 0x3f);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp, env,
            lsenv_offset_of_cc_src(lsenv));

    /* 2. set env->cc_op = 1 */
    latxs_append_ir2_opnd2i(LISA_ORI, &temp, zero, 1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp, env,
            lsenv_offset_of_cc_op(lsenv));
}

/*
 * Only called in :
 * 1. static context switch epilogue
 */
void latxs_tr_gen_static_load_eflags(int simple)
{
    /* Load cc_src(OF/SF/ZF/AF/PF/CF) into temp register */
    //IR2_OPND cc_src = latxs_stmp1_ir2_opnd;
    IR2_OPND cc_src = latxs_arg2_ir2_opnd;
    latxs_append_ir2_opnd2i(LISA_LD_W, &cc_src, &latxs_env_ir2_opnd,
            lsenv_offset_of_cc_src(lsenv));

    /* Move OF/SF/ZF/AF/PF/CF into CPU */
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cc_src, 0x3f);
}

bool latxs_translate_cld(IR1_INST *pir1)
{
    /*
     * df was placed in env->df
     * env->df : if DF: env->df = 1 else env->df = -1
     */
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ORI, &tmp,
            &latxs_zero_ir2_opnd, 1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &tmp,
            &latxs_env_ir2_opnd, lsenv_offset_of_df(lsenv));
    latxs_ra_free_temp(&tmp);

    return true;
}

bool latxs_translate_std(IR1_INST *pir1)
{
    /*
     * df was placed in env->df
     * env->df : if DF: env->df = 1 else env->df = -1
     */
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    latxs_load_imm64_to_ir2(&tmp, -1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &tmp,
            &latxs_env_ir2_opnd, lsenv_offset_of_df(lsenv));
    latxs_ra_free_temp(&tmp);

    return true;
}

bool latxs_translate_clc(IR1_INST *pir1)
{
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x1);
    return true;
}

bool latxs_translate_stc(IR1_INST *pir1)
{
    IR2_OPND cf_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ORI, &cf_opnd, &latxs_zero_ir2_opnd, 0x1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cf_opnd, 0x1);
    latxs_ra_free_temp(&cf_opnd);
    return true;
}

bool latxs_translate_cmc(IR1_INST *pir1)
{
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &temp, 0x1);
    latxs_append_ir2_opnd2i(LISA_XORI, &temp, &temp, 0x1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &temp, 0x1);
    latxs_ra_free_temp(&temp);
    return true;
}

bool latxs_translate_clac(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    CHECK_EXCP_CLAC(pir1);

    IR2_OPND eflags = latxs_ra_alloc_itemp();
    IR2_OPND mask = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_WU, &eflags, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));

    latxs_load_imm32_to_ir2(&mask, AC_MASK, EXMode_Z);
    latxs_append_ir2_opnd3(LISA_NOR, &mask, &mask, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_AND, &eflags, &eflags, &mask);
    latxs_append_ir2_opnd2i(LISA_ST_W, &eflags, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    return true;
}

bool latxs_translate_stac(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    CHECK_EXCP_STAC(pir1);

    IR2_OPND eflags = latxs_ra_alloc_itemp();
    IR2_OPND mask = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_WU, &eflags, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    latxs_load_imm32_to_ir2(&mask, AC_MASK, EXMode_Z);
    latxs_append_ir2_opnd3(LISA_OR, &eflags, &eflags, &mask);
    latxs_append_ir2_opnd2i(LISA_ST_W, &eflags, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    return true;
}

/* End of TB in system-mode */
bool latxs_translate_popf(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    CHECK_EXCP_POPF(pir1);

    int data_size = latxs_ir1_data_size(pir1);
    bool pushpop = get_pushpop_option(td);
    bool check_tf = !td->sys.tf && td->sys_eob_pir1 == pir1;

    if (td->sys.code64) {
        data_size = (data_size == 16) ? 16 : 64;
        lsassert(data_size == 16 || data_size == 64);
    } else {
        lsassert(data_size == 16 || data_size == 32);
    }

    /*
     * popf
     * ----------------------
     * > eflags <= MEM(SS:ESP)
     * > ESP    <= ESP + 2/4
     * ----------------------
     * 1. tmp   <= MEM(SS:ESP) : softmmu
     * 2. tmp   => eflags : helper_write_eflags
     * 3. ESP   <= ESP + 2/4
     */

    /* 1.1 build MEM(SS:ESP) */
    IR1_OPND mem_ir1_opnd;
    if (pushpop) {
        ir1_opnd_build_full_mem(&mem_ir1_opnd, data_size,
            X86_REG_SS, X86_REG_ESP, td->sp_patch, 0, 0);
    } else {
        ir1_opnd_build_full_mem(&mem_ir1_opnd, data_size,
            X86_REG_SS, X86_REG_ESP, 0, 0, 0);
    }

    /* 1.2 read data from stack   : might generate exception */
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    int ss_addr_size = latxs_get_sys_stack_addr_size();
    latxs_load_ir1_mem_to_ir2(&tmp,
            &mem_ir1_opnd, EXMode_Z, ss_addr_size);

    /*
     * 2. write into eflags
     *
     * void helper_write_eflags(
     *      CPUX86State *env,
     *      target_ulong t0,
     *      uint32_t update_mask)
     *
     * void cpu_load_eflags(CPUX86State *env, int eflags, int update_mask)
     * {
     *     CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
     *     CC_OP = CC_OP_EFLAGS;
     *     env->df = 1 - (2 * ((eflags >> 10) & 1));
     *     env->eflags = (env->eflags & ~update_mask) |
     *         (eflags & update_mask) | 0x2;
     * }
     */

    uint32_t eflags_mask = 0;
    if (td->sys.cpl == 0) {
        eflags_mask = TF_MASK | AC_MASK | ID_MASK | NT_MASK |
                      IF_MASK | IOPL_MASK;
    } else {
        if (td->sys.cpl <= td->sys.iopl) {
            eflags_mask = TF_MASK | AC_MASK | ID_MASK | NT_MASK |
                          IF_MASK;
        } else {
            eflags_mask = TF_MASK | AC_MASK | ID_MASK | NT_MASK;
        }
    }
    if (data_size == 16) {
        eflags_mask &= 0xffff;
    }

    IR2_OPND *env  = &latxs_env_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    IR2_OPND tmp2 = latxs_ra_alloc_itemp();
    IR2_OPND tmp3 = latxs_ra_alloc_itemp();

    /* 2.1 update cpazso (lscpu) */
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &tmp, 0x3f);

    /*
     * 2.2 update df (in env)
     *
     * eflags.df = 1 : env->df = -1 // 2 // -2 // -1
     * eflags.df = 0 : env->df =  1 // 0 //  0 //  1
     *
     * env->df = 1 - (2 * ((eflags >> 10) & 1));
     */
    latxs_append_ir2_opnd2i(LISA_SRLI_D, &tmp2, &tmp, 9);
    latxs_append_ir2_opnd2i(LISA_ANDI, &tmp2, &tmp2, 0x2);
    latxs_append_ir2_opnd3(LISA_SUB_D, &tmp2, zero, &tmp2);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp2, &tmp2, 1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &tmp2,
            env, offsetof(CPUX86State, df));

    /*
     * 2.3 update others (in env)
     *
     * env->eflags = (env->eflags & ~update_mask) |
     *               (eflags & update_mask) | 0x2;
     */
    IR2_OPCODE ld_eflags_op = LISA_LD_D;
    IR2_OPCODE st_eflags_op = LISA_ST_D;

    /* tmp2 = mask */
    latxs_load_imm32_to_ir2(&tmp2, eflags_mask, EXMode_Z);
    /* tmp = tmp(eflags) & mask */
    latxs_append_ir2_opnd3(LISA_AND, &tmp, &tmp, &tmp2);

    /* tmp3 = env->eflags */
    latxs_append_ir2_opnd2i(ld_eflags_op, &tmp3,
            env, offsetof(CPUX86State, eflags));
    /* tmp3 = env->eflags & ~mask */
    latxs_append_ir2_opnd3(LISA_ANDN, &tmp3, &tmp3, &tmp2);
    if (check_tf) {
        /* TF always comes from the stacked flags. */
        la_out2i(LISA_ANDI, &tmp2, &tmp, TF_MASK);
    }

    latxs_append_ir2_opnd3(LISA_OR, &tmp, &tmp, &tmp3);
    latxs_append_ir2_opnd2i(LISA_ORI, &tmp, &tmp, 0x2);

    latxs_append_ir2_opnd2i(st_eflags_op, &tmp,
            env, offsetof(CPUX86State, eflags));

    latxs_ra_free_temp(&tmp);
    latxs_ra_free_temp(&tmp3);

    /* 3. update ESP */
    int esp_inc = (data_size >> 3);
    if (pushpop) {
        td->sp_patch += esp_inc;
#ifdef CONFIG_LATX_PROFILER
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        per_tb_count((void *)&((tb->profile).pushpop_sub), 1);
#endif
    } else {
        latxs_update_stack_pointer(esp_inc, ss_addr_size);
    }

    if (check_tf) {
        IR2_OPND normal_exit = latxs_ir2_opnd_new_label();

        /* Setting TF changes the successor TB's lookup flags. */
        lsassert(!pushpop && !td->sp_patch);
        la_out2(LISA_BEQZ, &tmp2, &normal_exit);
        latxs_tr_gen_popf_no_link(pir1);
        latxs_append_ir2_opnd1(LISA_LABEL, &normal_exit);
    }

    latxs_ra_free_temp(&tmp2);
    return true;
}

bool latxs_translate_pushf(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    CHECK_EXCP_PUSHF(pir1);

    int data_size = latxs_ir1_data_size(pir1);
    bool pushpop = get_pushpop_option(td);

    if (td->sys.code64) {
        data_size = (data_size == 16) ? 16 : 64;
        lsassert(data_size == 16 || data_size == 64);
    } else {
        lsassert(data_size == 16 || data_size == 32);
    }

    /*
     * pushf
     * ----------------------
     * >  ESP    <= ESP - 2/4
     * >  eflags => MEM(SS:ESP)
     * ----------------------
     * 1. eflags <= eflags : mapping register
     * 2. tmp    => MEM(SS:ESP - 2/4) : softmmu
     * 3. ESP    <= ESP - 2/4
     */

    /*
     * 1. get the eflags
     *    env->cc_src : OF/SF/ZF/AF/PF/CF (mapped to la flag register)
     *    env->df     : if DF: env->df = 1 else env->df = -1
     *    env->eflags : every thing else except DF and OF/SF/ZF/AF/PF/CF
     */
    IR2_OPND ls_eflags = latxs_ra_alloc_itemp();
    /* 1.1 get OF/SF/ZF/AF/PF/CF */
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &ls_eflags,  0x3f);
    /* 1.2 get env->eflags */
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_WU, &temp, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    IR2_OPND mask = latxs_ra_alloc_itemp();
    /* clear df and oszapc flags, just avoid potential risk */
    latxs_append_ir2_opnd2i(LISA_ORI, &mask, &latxs_zero_ir2_opnd, 0xcd5);
    latxs_append_ir2_opnd3(LISA_NOR, &mask, &latxs_zero_ir2_opnd, &mask);
    latxs_append_ir2_opnd3(LISA_AND, &temp, &temp, &mask);
    latxs_ra_free_temp(&mask);
    latxs_append_ir2_opnd3(LISA_OR, &ls_eflags, &temp, &ls_eflags);
    /* 1.2 get DF */
    latxs_append_ir2_opnd2i(LISA_LD_WU, &temp, &latxs_env_ir2_opnd,
            lsenv_offset_of_df(lsenv));
    latxs_append_ir2_opnd2i(LISA_ANDI, &temp, &temp, 0x400);
    latxs_append_ir2_opnd3(LISA_OR, &ls_eflags, &temp, &ls_eflags);


    /* 2.1 build MEM(SS:ESP - 2/4) */
    int esp_dec = 0 - (data_size >> 3);
    IR1_OPND mem_ir1_opnd;
    if (pushpop) {
        td->sp_patch += esp_dec;
        ir1_opnd_build_full_mem(&mem_ir1_opnd, data_size,
                X86_REG_SS, X86_REG_ESP, td->sp_patch, 0, 0);
    } else {
        ir1_opnd_build_full_mem(&mem_ir1_opnd, data_size,
                X86_REG_SS, X86_REG_ESP, esp_dec, 0, 0);
    }

    /*
     * 2.2 write eflags into stack : might generate exception
     *     pushf will not write VM(17) and RF(16)
     */
    IR2_OPND eflags_to_push = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ORI, &eflags_to_push,
                                      &latxs_zero_ir2_opnd,  0x3);
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &eflags_to_push,
                                         &eflags_to_push, 0x10);
    latxs_append_ir2_opnd2_(lisa_not, &eflags_to_push, &eflags_to_push);

    latxs_append_ir2_opnd3(LISA_AND,
            &eflags_to_push, &ls_eflags, &eflags_to_push);

    int ss_addr_size = latxs_get_sys_stack_addr_size();
    latxs_store_ir2_to_ir1_mem(&eflags_to_push,
            &mem_ir1_opnd, ss_addr_size);
    latxs_ra_free_temp(&eflags_to_push);

    /* 3. update ESP */
    if (!pushpop) {
        latxs_update_stack_pointer(esp_dec, ss_addr_size);
    } else {
#ifdef CONFIG_LATX_PROFILER
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        per_tb_count((void *)&((tb->profile).pushpop_sub), 1);
#endif
    }

    return true;
}

/* End of TB in system-mode */
bool latxs_translate_clts(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    CHECK_EXCP_CLTS(pir1);

    /* void helper_clts(CPUX86State  *env) */

    IR2_OPND cr0 = latxs_ra_alloc_itemp();
    IR2_OPND hf = latxs_ra_alloc_itemp();

    int off_cr0 = lsenv_offset_of_cr(lsenv, 0);
    int off_hf  = lsenv_offset_of_hflags(lsenv);

    IR2_OPND *env = &latxs_env_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    latxs_append_ir2_opnd2i(LISA_LD_WU, &cr0, env, off_cr0);
    latxs_append_ir2_opnd2i(LISA_LD_WU, &hf, env, off_hf);

    IR2_OPND mask = latxs_ra_alloc_itemp();

    /*env->cr[0] &= ~CR0_TS_MASK;*/
    latxs_append_ir2_opnd2i(LISA_ORI, &mask, zero, CR0_TS_MASK);
    latxs_append_ir2_opnd3(LISA_NOR, &mask, zero, &mask);
    latxs_append_ir2_opnd3(LISA_AND, &cr0, &cr0, &mask);

    /*env->hflags &= ~HF_TS_MASK;*/
    latxs_append_ir2_opnd2i(LISA_ORI, &mask, zero, HF_TS_MASK);
    latxs_append_ir2_opnd3(LISA_NOR, &mask, zero, &mask);
    latxs_append_ir2_opnd3(LISA_AND, &hf, &hf, &mask);

    latxs_append_ir2_opnd2i(LISA_ST_W, &cr0, env, off_cr0);
    latxs_append_ir2_opnd2i(LISA_ST_W, &hf, env, off_hf);

    return true;
}

bool latxs_translate_sahf(IR1_INST *pir1)
{
    IR2_OPND ah = latxs_ra_alloc_itemp();
    latxs_load_ir1_gpr_to_ir2(&ah, &ah_ir1_opnd, EXMode_Z);
    latxs_append_ir2_opnd2i(LISA_ORI, &ah, &ah, 0x2);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &ah, 0x1f);
    latxs_ra_free_temp(&ah);
    return true;
}

bool latxs_translate_lahf(IR1_INST *pir1)
{
    IR2_OPND ah = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &ah, 0x1f);
    latxs_append_ir2_opnd2i(LISA_ORI, &ah, &ah, 0x2);
    latxs_store_ir2_to_ir1_gpr(&ah, &ah_ir1_opnd);
    latxs_ra_free_temp(&ah);
    return true;
}
