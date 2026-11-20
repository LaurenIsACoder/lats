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
#include "latx-sys-hamt-dmw.h"
#include "sys-aot.h"

#define CONFIG_ATOMIC_CAS_BH

#ifdef CONFIG_ATOMIC_CAS_BH

#define ATO_LISA_LD(opsz)   (opsz == 16 ? LISA_LD_H : LISA_LD_B)
#define ATO_LISA_CAS(opsz)  (opsz == 16 ? LISA_AMCAS_DB_H : LISA_AMCAS_DB_B)
#define ATO_LISA_AMCAS(opsz)  (opsz == 64 ? LISA_AMCAS_DB_D : \
                               opsz == 32 ? LISA_AMCAS_DB_W : \
                               opsz == 16 ? LISA_AMCAS_DB_H : LISA_AMCAS_DB_B)
#define ATO_LISA_AMADD(opsz)  (opsz == 64 ? LISA_AMADD_DB_D : \
                               opsz == 32 ? LISA_AMADD_DB_W : \
                               opsz == 16 ? LISA_AMADD_DB_H : LISA_AMADD_DB_B)
#define ATO_LISA_AMSWAP(opsz)  (opsz == 64 ? LISA_AMSWAP_DB_D : \
                                opsz == 32 ? LISA_AMSWAP_DB_W : \
                                opsz == 16 ? LISA_AMSWAP_DB_H : LISA_AMSWAP_DB_B)

#define ATO_AND_BH_ENABLE   (true)
#define ATO_OR_BH_ENABLE    (true)
#define ATO_XOR_BH_ENABLE   (true)
#define ATO_NOT_BH_ENABLE   (true)

#endif

#define LATXS_MOV_ADDR_TO_ARG(argx, src)                       \
    do {                                                       \
        if (latxs_ir1_addr_size(pir1) == 8) {                  \
            latxs_append_ir2_opnd2_(lisa_mov, &argx, &src);    \
        } else {                                               \
            latxs_append_ir2_opnd2_(lisa_mov32z, &argx, &src); \
        }                                                      \
    } while (0)

static inline int opsz2index(int opsz)
{
    /* 8->0, 16->1, 32->2, 64->3*/
    lsassert(opsz == 8 || opsz == 16 || opsz == 32 || opsz == 64);
    return __builtin_ctz(opsz) - 3;
}
static inline int opsz2tcgmemop(int opsz)
{
    /* sign and endian are irrelevant */
    static const int ops[] = {MO_8, MO_16, MO_32, MO_64};
    return ops[opsz2index(opsz)];
}

static ADDR atomis_add_functions[4] = {
    (ADDR)helper_atomic_fetch_addb,
    (ADDR)helper_atomic_fetch_addw_le,
    (ADDR)helper_atomic_fetch_addl_le,
    (ADDR)helper_atomic_fetch_addq_le,
};
static ADDR atomis_and_functions[4] = {
    (ADDR)helper_atomic_fetch_andb,
    (ADDR)helper_atomic_fetch_andw_le,
    (ADDR)helper_atomic_fetch_andl_le,
    (ADDR)helper_atomic_fetch_andq_le,
};
static ADDR atomis_or_functions[4] = {
    (ADDR)helper_atomic_fetch_orb,
    (ADDR)helper_atomic_fetch_orw_le,
    (ADDR)helper_atomic_fetch_orl_le,
    (ADDR)helper_atomic_fetch_orq_le,
};
static ADDR atomis_xor_functions[4] = {
    (ADDR)helper_atomic_fetch_xorb,
    (ADDR)helper_atomic_fetch_xorw_le,
    (ADDR)helper_atomic_fetch_xorl_le,
    (ADDR)helper_atomic_fetch_xorq_le,
};
static ADDR atomis_cmpxchg_functions[4] = {
    (ADDR)helper_atomic_cmpxchgb,
    (ADDR)helper_atomic_cmpxchgw_le,
    (ADDR)helper_atomic_cmpxchgl_le,
    (ADDR)helper_atomic_cmpxchgq_le,
};
static ADDR atomis_xchg_functions[4] = {
    (ADDR)helper_atomic_xchgb,
    (ADDR)helper_atomic_xchgw_le,
    (ADDR)helper_atomic_xchgl_le,
    (ADDR)helper_atomic_xchgq_le,
};
static int atomis_add_functions_id[4] = {
    HELPER_ATOMIC_FETCH_ADDB,
    HELPER_ATOMIC_FETCH_ADDW_LE,
    HELPER_ATOMIC_FETCH_ADDL_LE,
    HELPER_ATOMIC_FETCH_ADDQ_LE,
};
static int atomis_and_functions_id[4] = {
    HELPER_ATOMIC_FETCH_ANDB,
    HELPER_ATOMIC_FETCH_ANDW_LE,
    HELPER_ATOMIC_FETCH_ANDL_LE,
    HELPER_ATOMIC_FETCH_ANDQ_LE,
};
static int atomis_or_functions_id[4] = {
    HELPER_ATOMIC_FETCH_ORB,
    HELPER_ATOMIC_FETCH_ORW_LE,
    HELPER_ATOMIC_FETCH_ORL_LE,
    HELPER_ATOMIC_FETCH_ORQ_LE,
};
static int atomis_xor_functions_id[4] = {
    HELPER_ATOMIC_FETCH_XORB,
    HELPER_ATOMIC_FETCH_XORW_LE,
    HELPER_ATOMIC_FETCH_XORL_LE,
    HELPER_ATOMIC_FETCH_XORQ_LE,
};
static int atomis_cmpxchg_functions_id[4] = {
    HELPER_ATOMIC_CMPXCHGB,
    HELPER_ATOMIC_CMPXCHGW_LE,
    HELPER_ATOMIC_CMPXCHGL_LE,
    HELPER_ATOMIC_CMPXCHGQ_LE,
};
static int atomis_xchg_functions_id[4] = {
    HELPER_ATOMIC_XCHGB,
    HELPER_ATOMIC_XCHGW_LE,
    HELPER_ATOMIC_XCHGL_LE,
    HELPER_ATOMIC_XCHGQ_LE,
};

bool latxs_translate_lock_add(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        latxs_append_ir2_opnd3(ATO_LISA_AMADD(opsz), &tmp, &src0_mem_addr, &src1);
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        /* 8bit is always aligned */
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_sub(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        latxs_append_ir2_opnd3(LISA_SUB_D, &src1, &latxs_zero_ir2_opnd, &src1);
        latxs_append_ir2_opnd3(ATO_LISA_AMADD(opsz), &tmp, &src0_mem_addr, &src1);
        /* restore src1 to old valule */
        latxs_append_ir2_opnd3(LISA_SUB_D, &src1, &latxs_zero_ir2_opnd, &src1);
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        /* 8bit is always aligned */
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    latxs_append_ir2_opnd3(LISA_SUB_D, &src1, &latxs_zero_ir2_opnd, &src1);

    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    /* restore src1 to old valule */
    latxs_append_ir2_opnd3(LISA_SUB_D, &src1, &latxs_zero_ir2_opnd, &src1);
    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd3(LISA_SUB_D, &latxs_arg2_ir2_opnd,
                           &latxs_zero_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_adc(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_load_eflags_cf_to_ir2(&latxs_arg2_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_ADD_D, &latxs_arg2_ir2_opnd,
                           &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_sbb(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);

    latxs_load_eflags_cf_to_ir2(&latxs_arg2_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_SUB_D, &latxs_arg2_ir2_opnd,
                           &latxs_zero_ir2_opnd, &latxs_arg2_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_SUB_D, &latxs_arg2_ir2_opnd,
                           &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_inc(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);

    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        latxs_append_ir2_opnd2i(LISA_ORI, &src1, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd3(ATO_LISA_AMADD(opsz), &tmp, &src0_mem_addr, &src1);
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
     }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        /* 8bit is always aligned */
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    latxs_append_ir2_opnd2i(LISA_ORI, &src1, &latxs_zero_ir2_opnd, 1);
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_load_imm64_to_ir2(&latxs_arg2_ir2_opnd, 1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &latxs_zero_ir2_opnd, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    return true;
}
bool latxs_translate_lock_dec(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);

    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &src1, &latxs_zero_ir2_opnd, -1);
        latxs_append_ir2_opnd3(ATO_LISA_AMADD(opsz), &tmp, &src0_mem_addr, &src1);
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        /* 8bit is always aligned */
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &src1, &latxs_zero_ir2_opnd, -1);
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_load_imm64_to_ir2(&latxs_arg2_ir2_opnd, -1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &latxs_zero_ir2_opnd, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    return true;
}
bool latxs_translate_lock_xadd(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        latxs_append_ir2_opnd3(ATO_LISA_AMADD(opsz), &tmp, &src0_mem_addr, &src1);
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_store_ir2_to_ir1(&tmp, opnd1);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        /* 8bit is always aligned */
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_H, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMADD_DB_B, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
    latxs_store_ir2_to_ir1(&tmp, opnd1);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_add_functions[opsz2index(opsz)],
            atomis_add_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);
    latxs_store_ir2_to_ir1(&latxs_ret0_ir2_opnd, opnd1);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_and(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
#ifdef CONFIG_ATOMIC_CAS_BH
    bool use_amcas_bh = false;
#endif

    if (do_fast_atomic) {
        if (opsz == 64) {
            latxs_append_ir2_opnd3(LISA_AMAND_DB_D, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 32) {
            latxs_append_ir2_opnd3(LISA_AMAND_DB_W, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 16 || opsz == 8) {
            IR2_OPND label_atomic_retry = la_new_label();
            la_label_name(label_atomic_retry, "atomic AND retry");
            /* 1. read source operand */
            IR2_OPND src0 = la_new_itmp();
            la_out2i(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0);
            la_out2_(lisa_mov, &tmp, &src0);
            /* 2. do operation (and) */
            IR2_OPND res  = la_new_itmp();
            la_out3(LISA_AND, &res, &src0, &src1);
            /* 3. ret = AMCAS */
            latxs_append_ir2_opnd3(ATO_LISA_CAS(opsz), &src0, &src0_mem_addr, &res);
            /* 4. compare source with ret. goto 1 if not equal */
            la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

            la_tmp_free(res);
            la_tmp_free(src0);
            if (!(cpuinfo & CPUINFO_LAM_BH)) {
                lsassert(0);
            }
        } else {
            lsassert(0);
        }
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else {
#ifndef CONFIG_ATOMIC_CAS_BH
        /* some am not support 8bit 16bit, goto atomic helper */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
#else
        /*
         * use AMCAS_BH to implement 8bit 16bit atomic inst
         * do align checking herer
         */
        if (!(cpuinfo & CPUINFO_LAMCAS) || !(ATO_AND_BH_ENABLE)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        } else {
            use_amcas_bh = true;
            if (opsz == 16) {
                la_out2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
                la_out3 (LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
            }
            /* 8bit is always aligned */
        }
#endif
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMAND_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMAND_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMAND_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMAND_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    }
#ifdef CONFIG_ATOMIC_CAS_BH
    else if (use_amcas_bh && ATO_AND_BH_ENABLE) {
        IR2_OPND label_atomic_retry = la_new_label();
        la_label_name(label_atomic_retry, "atomic AND retry");
        /* 1. read source operand */
        IR2_OPND src0 = la_new_itmp();
        gen_ldst_softmmu_helper(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0, 1);
        la_out2_(lisa_mov, &tmp, &src0);
        /* 2. do operation (and) */
        IR2_OPND res  = la_new_itmp();
        la_out3(LISA_AND, &res, &src0, &src1);
        /* 3. ret = AMCAS */
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr);
#endif
        /* 4. compare source with ret. goto 1 if not equal */
        la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

        la_tmp_free(res);
        la_tmp_free(src0);
    }
#endif

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_and_functions[opsz2index(opsz)],
            atomis_and_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_or(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
#ifdef CONFIG_ATOMIC_CAS_BH
    bool use_amcas_bh = false;
#endif
    if (do_fast_atomic) {
        if (opsz == 64) {
            latxs_append_ir2_opnd3(LISA_AMOR_DB_D, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 32) {
            latxs_append_ir2_opnd3(LISA_AMOR_DB_W, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 16 || opsz == 8) {
            IR2_OPND label_atomic_retry = la_new_label();
            la_label_name(label_atomic_retry, "atomic OR retry");
            /* 1. read source operand */
            IR2_OPND src0 = la_new_itmp();
            la_out2i(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0);
            la_out2_(lisa_mov, &tmp, &src0);
            /* 2. do operation (and) */
            IR2_OPND res  = la_new_itmp();
            la_out3(LISA_OR, &res, &src0, &src1);
            /* 3. ret = AMCAS */
            latxs_append_ir2_opnd3(ATO_LISA_CAS(opsz), &src0, &src0_mem_addr, &res);
            /* 4. compare source with ret. goto 1 if not equal */
            la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

            la_tmp_free(res);
            la_tmp_free(src0);
            if (!(cpuinfo & CPUINFO_LAM_BH)) {
                lsassert(0);
            }
        } else {
            lsassert(0);
        }
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }

    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else {
#ifndef CONFIG_ATOMIC_CAS_BH
        /* some am not support 8bit 16bit, goto atomic helper */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
#else
        /*
         * use AMCAS_BH to implement 8bit 16bit atomic inst
         * do align checking herer
         */
        if (!(cpuinfo & CPUINFO_LAMCAS) || !(ATO_OR_BH_ENABLE)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        } else {
            use_amcas_bh = true;
            if (opsz == 16) {
                la_out2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
                la_out3 (LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
            }
            /* 8bit is always aligned */
        }
#endif
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMOR_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMOR_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMOR_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMOR_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    }
#ifdef CONFIG_ATOMIC_CAS_BH
    else if (use_amcas_bh && ATO_OR_BH_ENABLE) {
        IR2_OPND label_atomic_retry = la_new_label();
        la_label_name(label_atomic_retry, "atomic AND retry");
        /* 1. read source operand */
        IR2_OPND src0 = la_new_itmp();
        gen_ldst_softmmu_helper(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0, 1);
        la_out2_(lisa_mov, &tmp, &src0);
        /* 2. do operation (and) */
        IR2_OPND res  = la_new_itmp();
        la_out3(LISA_OR, &res, &src0, &src1);
        /* 3. ret = AMCAS */
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr);
#endif
        /* 4. compare source with ret. goto 1 if not equal */
        la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

        la_tmp_free(res);
        la_tmp_free(src0);
    }
#endif

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif

    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_or_functions[opsz2index(opsz)],
            atomis_or_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_xor(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);
    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
#ifdef CONFIG_ATOMIC_CAS_BH
    bool use_amcas_bh = false;
#endif
    if (do_fast_atomic) {
        if (opsz == 64) {
            latxs_append_ir2_opnd3(LISA_AMXOR_DB_D, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 32) {
            latxs_append_ir2_opnd3(LISA_AMXOR_DB_W, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 16 || opsz == 8) {
            IR2_OPND label_atomic_retry = la_new_label();
            la_label_name(label_atomic_retry, "atomic XOR retry");
            /* 1. read source operand */
            IR2_OPND src0 = la_new_itmp();
            la_out2i(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0);
            la_out2_(lisa_mov, &tmp, &src0);
            /* 2. do operation (and) */
            IR2_OPND res  = la_new_itmp();
            la_out3(LISA_XOR, &res, &src0, &src1);
            /* 3. ret = AMCAS */
            latxs_append_ir2_opnd3(ATO_LISA_CAS(opsz), &src0, &src0_mem_addr, &res);
            /* 4. compare source with ret. goto 1 if not equal */
            la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

            la_tmp_free(res);
            la_tmp_free(src0);
            if (!(cpuinfo & CPUINFO_LAM_BH)) {
                lsassert(0);
            }
        } else {
            lsassert(0);
        }
        latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }

    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else {
#ifndef CONFIG_ATOMIC_CAS_BH
        /* some am not support 8bit 16bit, goto atomic helper */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
#else
        /*
         * use AMCAS_BH to implement 8bit 16bit atomic inst
         * do align checking herer
         */
        if (!(cpuinfo & CPUINFO_LAMCAS) || !(ATO_XOR_BH_ENABLE)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        } else {
            use_amcas_bh = true;
            if (opsz == 16) {
                la_out2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
                la_out3 (LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
            }
            /* 8bit is always aligned */
        }
#endif
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMXOR_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMXOR_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    }
#ifdef CONFIG_ATOMIC_CAS_BH
    else if (use_amcas_bh && ATO_XOR_BH_ENABLE) {
        IR2_OPND label_atomic_retry = la_new_label();
        la_label_name(label_atomic_retry, "atomic AND retry");
        /* 1. read source operand */
        IR2_OPND src0 = la_new_itmp();
        gen_ldst_softmmu_helper(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0, 1);
        la_out2_(lisa_mov, &tmp, &src0);
        /* 2. do operation (and) */
        IR2_OPND res  = la_new_itmp();
        la_out3(LISA_XOR, &res, &src0, &src1);
        /* 3. ret = AMCAS */
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr);
#endif
        /* 4. compare source with ret. goto 1 if not equal */
        la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

        la_tmp_free(res);
        la_tmp_free(src0);
    }
#endif

    latxs_generate_eflag_calculation(NULL, &tmp, &src1, pir1, true);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif

    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_xor_functions[opsz2index(opsz)],
            atomis_xor_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_ra_free_temp(&src1);
    src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_ret0_ir2_opnd,
                                     &src1, pir1, true);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_not(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();
#ifdef CONFIG_ATOMIC_CAS_BH
    bool use_amcas_bh = false;
#endif
    if (do_fast_atomic) {
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &src1, &latxs_zero_ir2_opnd, -1);
        if (opsz == 64) {
            latxs_append_ir2_opnd3(LISA_AMXOR_DB_D, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 32) {
            latxs_append_ir2_opnd3(LISA_AMXOR_DB_W, &tmp, &src0_mem_addr, &src1);
        } else if (opsz == 16 || opsz == 8) {
            IR2_OPND label_atomic_retry = la_new_label();
            la_label_name(label_atomic_retry, "atomic XOR retry");
            /* 1. read source operand */
            IR2_OPND src0 = la_new_itmp();
            la_out2i(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0);
            la_out2_(lisa_mov, &tmp, &src0);
            /* 2. do operation (and) */
            IR2_OPND res  = la_new_itmp();
            la_out3(LISA_XOR, &res, &src0, &src1);
            /* 3. ret = AMCAS */
            latxs_append_ir2_opnd3(ATO_LISA_CAS(opsz), &src0, &src0_mem_addr, &res);
            /* 4. compare source with ret. goto 1 if not equal */
            la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

            la_tmp_free(res);
            la_tmp_free(src0);
            if (!(cpuinfo & CPUINFO_LAM_BH)) {
                lsassert(0);
            }
        } else {
            lsassert(0);
        }
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }

    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else {
#ifndef CONFIG_ATOMIC_CAS_BH
        /* some am not support 8bit 16bit, goto atomic helper */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
#else
        /*
         * use AMCAS_BH to implement 8bit 16bit atomic inst
         * do align checking herer
         */
        if (!(cpuinfo & CPUINFO_LAMCAS) || !(ATO_NOT_BH_ENABLE)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        } else {
            use_amcas_bh = true;
            if (opsz == 16) {
                la_out2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
                la_out3 (LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
            }
            /* 8bit is always aligned */
        }
#endif
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &src1, &latxs_zero_ir2_opnd, -1);

    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMXOR_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMXOR_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    }
#ifdef CONFIG_ATOMIC_CAS_BH
    else if (use_amcas_bh && ATO_NOT_BH_ENABLE) {
        IR2_OPND label_atomic_retry = la_new_label();
        la_label_name(label_atomic_retry, "atomic AND retry");
        /* 1. read source operand */
        IR2_OPND src0 = la_new_itmp();
        gen_ldst_softmmu_helper(ATO_LISA_LD(opsz), &src0, &src0_mem_addr, 0, 1);
        la_out2_(lisa_mov, &tmp, &src0);
        /* 2. do operation (and) */
        IR2_OPND res  = la_new_itmp();
        la_out3(LISA_XOR, &res, &src0, &src1);
        /* 3. ret = AMCAS */
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(ATO_LISA_CAS(opsz), &src0, &res, &src0_mem_addr);
#endif
        /* 4. compare source with ret. goto 1 if not equal */
        la_out3(LISA_BNE, &tmp, &src0, &label_atomic_retry);

        la_tmp_free(res);
        la_tmp_free(src0);
    }
#endif

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_load_imm64_to_ir2(&latxs_arg2_ir2_opnd, -1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_xor_functions[opsz2index(opsz)],
            atomis_xor_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    return true;
}

bool latxs_translate_lock_neg(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int opsz = ir1_opnd_size(opnd0);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src0 = latxs_ra_alloc_itemp();
    IR2_OPND neg = latxs_ra_alloc_itemp();
    IR2_OPND label_unequal = latxs_ir2_opnd_new_label();

    latxs_append_ir2_opnd1(LISA_LABEL, &label_unequal);

    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src0, opnd0, EXMode_S);
    latxs_append_ir2_opnd3(LISA_SUB_D, &neg, &latxs_zero_ir2_opnd, &src0);

    latxs_tr_save_temp_register_mask(lsenv->tr_data->itemp_mask);
    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src0);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg3_ir2_opnd, &neg);
    latxs_load_imm64_to_ir2(
        &latxs_arg4_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper(
        (ADDR)atomis_cmpxchg_functions[opsz2index(opsz)],
        atomis_cmpxchg_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
    latxs_tr_restore_temp_register_mask(lsenv->tr_data->itemp_mask);

    if (opsz == 8) {
        latxs_append_ir2_opnd2_(lisa_mov8s, &latxs_ret0_ir2_opnd,
                                &latxs_ret0_ir2_opnd);
    } else if (opsz == 16) {
        latxs_append_ir2_opnd2_(lisa_mov16s, &latxs_ret0_ir2_opnd,
                                &latxs_ret0_ir2_opnd);
    }

    latxs_append_ir2_opnd3(LISA_BNE, &latxs_ret0_ir2_opnd, &src0,
                           &label_unequal);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd,
                                     &latxs_ret0_ir2_opnd, pir1, true);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src0);
    latxs_ra_free_temp(&neg);
    return true;
}
bool latxs_translate_lock_btx(IR1_INST *pir1)
{
    lsassert(ir1_opcode(pir1) != X86_INS_BT);
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* bit base   : GPR/MEM */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* bit offset : GPR/imm */
    int opsz = ir1_opnd_size(opnd0);
    lsassert(opsz == 16 || opsz == 32 || opsz == 64);

    IR2_OPND bit_off = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    IR2_OPND mem = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&mem, opnd0);

    /* opnd0 is memory operand */
    if (ir1_opnd_is_imm(opnd1)) {
        latxs_load_imm64_to_ir2(&bit_off, ir1_opnd_uimm(opnd1) & (opsz - 1));
    } else {
        IR2_OPND byte_nr = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_S);
        latxs_append_ir2_opnd2i(LISA_SRAI_D, &byte_nr, &bit_off,
                                __builtin_ctz(opsz));
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &byte_nr, &byte_nr,
                                opsz2index(opsz));
        latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, opsz - 1);

        latxs_append_ir2_opnd3(LISA_ADD_D, &mem, &mem, &byte_nr);
        latxs_ra_free_temp(&byte_nr);
    }

    IR2_OPND bit_set = latxs_ra_alloc_itemp();

    latxs_load_imm64_to_ir2(&bit_set, 1);
    latxs_append_ir2_opnd3(LISA_SLL_D, &bit_set, &bit_set, &bit_off);
    if (ir1_opcode(pir1) == X86_INS_BTR) {
        latxs_append_ir2_opnd2_(lisa_not, &bit_set, &bit_set);
    }

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    if (do_fast_atomic && (opsz == 64 || opsz == 32)) {
        if (opsz == 64) {
            switch (ir1_opcode(pir1)) {
            case X86_INS_BTS: /* set bit = 1 */
                latxs_append_ir2_opnd3(LISA_AMOR_DB_D, &tmp, &mem, &bit_set);
                break;
            case X86_INS_BTR: /* set bit = 0 */
                latxs_append_ir2_opnd3(LISA_AMAND_DB_D, &tmp, &mem, &bit_set);
                break;
            case X86_INS_BTC: /* set bit = not bit  */
                latxs_append_ir2_opnd3(LISA_AMXOR_DB_D, &tmp, &mem, &bit_set);
                break;
            default:
                lsassertm(0, "Invalid opcode in translate_btx\n");
                break;
            }
        } else {
            /* (opsz == 32) */
            switch (ir1_opcode(pir1)) {
            case X86_INS_BTS: /* set bit = 1 */
                latxs_append_ir2_opnd3(LISA_AMOR_DB_W, &tmp, &mem, &bit_set);
                break;
            case X86_INS_BTR: /* set bit = 0 */
                latxs_append_ir2_opnd3(LISA_AMAND_DB_W, &tmp, &mem, &bit_set);
                break;
            case X86_INS_BTC: /* set bit = not bit  */
                latxs_append_ir2_opnd3(LISA_AMXOR_DB_W, &tmp, &mem, &bit_set);
                break;
            default:
                lsassertm(0, "Invalid opcode in translate_btx\n");
                break;
            }
        }
        if (ir1_opnd_is_imm(opnd1)) {
            latxs_load_imm64_to_ir2(&bit_off, ir1_opnd_uimm(opnd1) & (opsz - 1));
        } else {
            latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_S);
            latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, opsz - 1);
        }

        latxs_append_ir2_opnd3(LISA_SRL_D, &tmp, &tmp, &bit_off);
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &tmp, 1);
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &tmp, 0x1);
        return true;
    }
    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &mem, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &mem, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else {
        /* some am not support 8bit 16bit, goto atomic helper */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
    }

    switch (ir1_opcode(pir1)) {
    case X86_INS_BTS: /* set bit = 1 */
        if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMOR_DB_D, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMOR_DB_D, &tmp, &bit_set, &mem);
#endif
        } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMOR_DB_W, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMOR_DB_W, &tmp, &bit_set, &mem);
#endif
        }
        break;
    case X86_INS_BTR: /* set bit = 0 */
        if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMAND_DB_D, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMAND_DB_D, &tmp, &bit_set, &mem);
#endif
        } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMAND_DB_W, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMAND_DB_W, &tmp, &bit_set, &mem);
#endif
        }
        break;
    case X86_INS_BTC: /* set bit = not bit  */
        if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_D, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMXOR_DB_D, &tmp, &bit_set, &mem);
#endif
        } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
            tr_gen_atomic_lookup_qemu_tlb(LISA_AMXOR_DB_W, &tmp, &bit_set, &mem, mmu_index,
                                is_load, &label_atomic_helper);
#else
            la_out3(LISA_AMXOR_DB_W, &tmp, &bit_set, &mem);
#endif
        }
        break;
    default:
        lsassertm(0, "Invalid opcode in translate_btx\n");
        break;
    }

    if (ir1_opnd_is_imm(opnd1)) {
        latxs_load_imm64_to_ir2(&bit_off, ir1_opnd_uimm(opnd1) & (opsz - 1));
    } else {
        latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_S);
        latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, opsz - 1);
    }

    latxs_append_ir2_opnd3(LISA_SRL_D, &tmp, &tmp, &bit_off);
    latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &tmp, 1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &tmp, 0x1);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, mem);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &bit_set);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);
    /* update BitBase */
    switch (ir1_opcode(pir1)) {
    case X86_INS_BTS: /* set bit = 1 */
        latxs_tr_gen_call_to_helper(
            (ADDR)atomis_or_functions[opsz2index(opsz)],
            atomis_or_functions_id[opsz2index(opsz)]);
        break;
    case X86_INS_BTR: /* set bit = 0 */
        latxs_tr_gen_call_to_helper(
            (ADDR)atomis_and_functions[opsz2index(opsz)],
            atomis_and_functions_id[opsz2index(opsz)]);
        break;
    case X86_INS_BTC: /* set bit = not bit  */
        latxs_tr_gen_call_to_helper(
            (ADDR)atomis_xor_functions[opsz2index(opsz)],
            atomis_xor_functions_id[opsz2index(opsz)]);
        break;
    default:
        lsassertm(0, "Invalid opcode in translate_btx\n");
        break;
    }
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    if (ir1_opnd_is_imm(opnd1)) {
        latxs_load_imm64_to_ir2(&bit_off, ir1_opnd_uimm(opnd1) & (opsz - 1));
    } else {
        latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_S);
        latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, opsz - 1);
    }

    latxs_append_ir2_opnd3(LISA_SRL_D, &latxs_ret0_ir2_opnd,
                           &latxs_ret0_ir2_opnd, &bit_off);
    latxs_append_ir2_opnd2i(LISA_ANDI, &latxs_ret0_ir2_opnd,
                            &latxs_ret0_ir2_opnd, 1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_ret0_ir2_opnd, 0x1);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&bit_set);
    latxs_ra_free_temp(&bit_off);
    return true;
}

bool latxs_translate_lock_cmpxchg(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int opsz = ir1_opnd_size(opnd0);

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    IR1_OPND *reg_ir1 = NULL;

    int opnd_size = ir1_opnd_size(opnd0);
    switch (opnd_size) {
    case 8:
        reg_ir1 = &al_ir1_opnd;
        break;
    case 16:
        reg_ir1 = &ax_ir1_opnd;
        break;
    case 32:
        reg_ir1 = &eax_ir1_opnd;
        break;
    case 64:
        lsassert(lsenv->tr_data->sys.code64);
        reg_ir1 = &rax_ir1_opnd;
        break;
    default:
        lsassert(0);
        break;
    }

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();
    IR2_OPND eax_opnd = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);
    latxs_load_ir1_to_ir2(&eax_opnd, reg_ir1, EXMode_S);

    IR2_OPND old_eax = latxs_ra_alloc_itemp();
#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif

    if (!(cpuinfo & CPUINFO_LAMCAS)) {
        goto cmpxchg_helper;
    }

    if (do_fast_atomic) {
        latxs_append_ir2_opnd3(ATO_LISA_AMCAS(opsz), &eax_opnd, &src0_mem_addr, &src1);
        latxs_load_ir1_to_ir2(&old_eax, reg_ir1, EXMode_S);
        latxs_generate_eflag_calculation(NULL, &old_eax, &eax_opnd, pir1, true);
        IR2_OPND label_eax_nochg = latxs_ir2_opnd_new_label();
        // old_eax and eax_opnd are both signed extension
        if (opsz == 32) la_out3(LISA_BEQ, &old_eax, &eax_opnd, &label_eax_nochg);
        latxs_store_ir2_to_ir1(&eax_opnd, reg_ir1);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_eax_nochg);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &old_eax, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &old_eax, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &old_eax, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &old_eax, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &old_eax, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &old_eax, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 8) {
        /* opsz == 8 always aligned */
    } else {
        /* some am not support 8bit 16bit, goto atomic helper */
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMCAS_DB_D, &eax_opnd, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMCAS_DB_D, &eax_opnd, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMCAS_DB_W, &eax_opnd, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMCAS_DB_W, &eax_opnd, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMCAS_DB_H, &eax_opnd, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMCAS_DB_H, &eax_opnd, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMCAS_DB_B, &eax_opnd, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMCAS_DB_B, &eax_opnd, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    /* amcas always change rd to rj (change eax_opnd to *src0_mem_addr), 
     * so we need to generate EFLAGS.ZF by comparing old eax and new eax(src0_mem_addr) */
    latxs_load_ir1_to_ir2(&old_eax, reg_ir1, EXMode_S);
    latxs_generate_eflag_calculation(NULL, &old_eax, &eax_opnd, pir1, true);
    IR2_OPND label_eax_nochg_2 = latxs_ir2_opnd_new_label();
    if (opsz == 32) la_out3(LISA_BEQ, &old_eax, &eax_opnd, &label_eax_nochg_2);
    latxs_store_ir2_to_ir1(&eax_opnd, reg_ir1);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_eax_nochg_2);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);

cmpxchg_helper:
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &eax_opnd);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg3_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg4_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper(
        (ADDR)atomis_cmpxchg_functions[opsz2index(opsz)],
        atomis_cmpxchg_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* temp registers were not saved */
    latxs_load_ir1_to_ir2(&eax_opnd, reg_ir1, EXMode_S);
    latxs_generate_eflag_calculation(&latxs_zero_ir2_opnd, &eax_opnd,
                                     &latxs_ret0_ir2_opnd, pir1, true);

    IR2_OPND label_eax_nochg = latxs_ir2_opnd_new_label();
    if (opsz == 32) {
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &old_eax, &eax_opnd, 31, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &eax_opnd, &latxs_ret0_ir2_opnd, 31, 0);
        latxs_append_ir2_opnd3(LISA_BEQ, &eax_opnd, &old_eax, &label_eax_nochg);
    }
    latxs_store_ir2_to_ir1_gpr(&latxs_ret0_ir2_opnd, reg_ir1);
    if (opsz == 32) {
        latxs_append_ir2_opnd1(LISA_LABEL, &label_eax_nochg);
    }

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
bool latxs_translate_lock_cmpxchg8b(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

#ifdef CONFIG_ATOMIC
    IR2_OPND ir2_eax = latxs_ra_alloc_itemp();
    IR2_OPND ir2_edx = latxs_ra_alloc_itemp();

    IR2_OPND ir2_ecx = latxs_ra_alloc_itemp();
    IR2_OPND ir2_ebx = latxs_ra_alloc_itemp();

    IR2_OPND label_unequal = latxs_ir2_opnd_new_label();
    IR2_OPND label_end_eflags = latxs_ir2_opnd_new_label();

#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif

    if (!(cpuinfo & CPUINFO_LAMCAS)) {
        goto cmpxchg8b_helper;
    }

    if (!do_fast_atomic) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &ir2_eax, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &ir2_eax, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }

    /* load edx:eax */
    latxs_load_ir1_gpr_to_ir2(&ir2_eax, &eax_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_edx, &edx_ir1_opnd, EXMode_Z);
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &ir2_edx, &ir2_edx, 32);
    latxs_append_ir2_opnd3(LISA_OR, &ir2_edx, &ir2_edx, &ir2_eax);

    /* load ecx:ebx */
    latxs_load_ir1_gpr_to_ir2(&ir2_ecx, &ecx_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_ebx, &ebx_ir1_opnd, EXMode_Z);
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &ir2_ecx, &ir2_ecx, 32);
    latxs_append_ir2_opnd3(LISA_OR, &ir2_ecx, &ir2_ecx, &ir2_ebx);

    if (!do_fast_atomic) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMCAS_DB_D, &ir2_edx, &ir2_ecx, &src0_mem_addr, mmu_index,
                                      is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMCAS_DB_D, &ir2_edx, &ir2_ecx, &src0_mem_addr);
#endif
    } else {
        latxs_append_ir2_opnd3(LISA_AMCAS_DB_D, &ir2_edx, &src0_mem_addr, &ir2_ecx);
    }

    /* load old edx:eax to compare and generate EFLAGS.ZF*/
    latxs_load_ir1_gpr_to_ir2(&ir2_ebx, &eax_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_ecx, &edx_ir1_opnd, EXMode_Z);

    latxs_append_ir2_opnd2i(LISA_SLLI_D, &ir2_ecx, &ir2_ecx, 32);
    latxs_append_ir2_opnd3(LISA_OR, &ir2_ecx, &ir2_ecx, &ir2_ebx);

    /* update EFLAGS.ZF */
    latxs_append_ir2_opnd3(LISA_BNE, &ir2_edx, &ir2_ecx, &label_unequal);

    /* unequal ZF = 1 */
    latxs_append_ir2_opnd2i(LISA_ORI, &ir2_ebx, &latxs_zero_ir2_opnd, ZF_BIT);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &ir2_ebx, ZF_USEDEF_BIT);
    latxs_append_ir2_opnd1(LISA_B, &label_end_eflags);

    /* unequal ZF = 0 */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_unequal);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, ZF_USEDEF_BIT);

    /* split ir2 and store into ir1 */
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &ir2_eax, &ir2_edx, 31, 0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &ir2_edx, &ir2_edx, 63, 32);
    latxs_store_ir2_to_ir1(&ir2_eax, &eax_ir1_opnd);
    latxs_store_ir2_to_ir1(&ir2_edx, &edx_ir1_opnd);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_end_eflags);

    if (do_fast_atomic) {
        latxs_ra_free_temp(&src0_mem_addr);
        return true;
    } else {
        /* 2. memory access finish. jump to exit */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
    }

cmpxchg8b_helper:
#endif

    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_tr_gen_call_to_helper((ADDR)helper_latx_cmpxchg8b,
            HELPER_LATX_CMPXCHG8B);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    return true;
}
bool latxs_translate_lock_cmpxchg16b(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd0);

    if (!(cpuinfo & CPUINFO_SCQ)) {
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
    }

#ifdef CONFIG_ATOMIC
    IR2_OPND ir2_rax = latxs_ra_alloc_itemp();
    IR2_OPND ir2_rdx = latxs_ra_alloc_itemp();

    IR2_OPND ir2_rcx = latxs_ra_alloc_itemp();
    IR2_OPND ir2_rbx = latxs_ra_alloc_itemp();

    IR2_OPND label_loop = latxs_ir2_opnd_new_label();
    IR2_OPND label_end_loop = latxs_ir2_opnd_new_label();

    IR2_OPND label_unequal = latxs_ir2_opnd_new_label();

#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif

    if (!do_fast_atomic) {
        /* address aligned check */
        latxs_append_ir2_opnd2i(LISA_ANDI, &ir2_rax, &src0_mem_addr, 0xf);
        latxs_append_ir2_opnd3(LISA_BEQ, &ir2_rax, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_loop);

    /* load old rdx:rax */
    latxs_load_ir1_gpr_to_ir2(&ir2_rbx, &rax_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_rcx, &rdx_ir1_opnd, EXMode_Z);

    if (!do_fast_atomic) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_LL_D, &ir2_rax, NULL, &src0_mem_addr, mmu_index,
                                      is_load, &label_atomic_helper);
        /* LD will add 8 offset unconditionally, so tr_gen_atomic_lookup_qemu_tlb could not be used for other function */
        tr_gen_atomic_lookup_qemu_tlb(LISA_LD_D, &ir2_rdx, NULL, &src0_mem_addr, mmu_index,
                                      is_load, &label_atomic_helper);
#else
        la_out2i(LISA_LL_D, &ir2_rax, &src0_mem_addr, 0);
        la_out2i(LISA_LD_D, &ir2_rdx, &src0_mem_addr, 0);
#endif
    } else {
        latxs_append_ir2_opnd2i(LISA_LL_D, &ir2_rax, &src0_mem_addr, 0);
        latxs_append_ir2_opnd2i(LISA_LD_D, &ir2_rdx, &src0_mem_addr, 8);
    }

    /* ir2_rcx:ir2_rbx is old rdx:rax */
    latxs_append_ir2_opnd3(LISA_BNE, &ir2_rbx, &ir2_rax, &label_end_loop);
    latxs_append_ir2_opnd3(LISA_BNE, &ir2_rcx, &ir2_rdx, &label_end_loop);

    /* load rcx:rbx */
    latxs_load_ir1_gpr_to_ir2(&ir2_rbx, &rbx_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_rcx, &rcx_ir1_opnd, EXMode_Z);

    if (!do_fast_atomic) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_SC_Q, &ir2_rbx, &ir2_rcx, &src0_mem_addr, mmu_index,
                                      is_load, &label_atomic_helper);
#else
        la_out3(LISA_SC_Q, &ir2_rbx, &ir2_rcx, &src0_mem_addr);
#endif
    } else {
        latxs_append_ir2_opnd3(LISA_SC_Q, &ir2_rbx, &src0_mem_addr, &ir2_rcx);
    }

    latxs_append_ir2_opnd3(LISA_BEQ, &ir2_rbx, &latxs_zero_ir2_opnd, &label_loop);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_end_loop);

    /* load old edx:eax to compare and generate EFLAGS.ZF*/
    latxs_load_ir1_gpr_to_ir2(&ir2_rbx, &rax_ir1_opnd, EXMode_Z);
    latxs_load_ir1_gpr_to_ir2(&ir2_rcx, &rdx_ir1_opnd, EXMode_Z);

    /* rdx:rax is new value */
    latxs_store_ir2_to_ir1(&ir2_rax, &rax_ir1_opnd);
    latxs_store_ir2_to_ir1(&ir2_rdx, &rdx_ir1_opnd);

    /* update EFLAGS.ZF */
    latxs_append_ir2_opnd3(LISA_BNE, &ir2_rax, &ir2_rbx, &label_unequal);
    latxs_append_ir2_opnd3(LISA_BNE, &ir2_rdx, &ir2_rcx, &label_unequal);

    /* unequal ZF = 1 */
    latxs_append_ir2_opnd2i(LISA_ORI, &ir2_rdx, &latxs_zero_ir2_opnd, ZF_BIT);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &ir2_rdx, ZF_USEDEF_BIT);
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);

    /* unequal ZF = 0 */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_unequal);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, ZF_USEDEF_BIT);

    if (do_fast_atomic) {
        latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);
        latxs_ra_free_temp(&src0_mem_addr);
        return true;
    } else {
        /* 2. memory access finish. jump to exit */
        latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
    }
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_tr_gen_call_to_helper((ADDR)helper_latx_cmpxchg16b, HELPER_LATX_CMPXCHG16B);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    return true;
}
bool latxs_translate_lock_xchg(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

#ifdef CONFIG_ATOMIC
    int do_fast_atomic = option_fast_atomic;
#ifdef CONFIG_SOFTMMU
    if (!can_do_hamt(lsenv->cpu_state)) {
        do_fast_atomic = 0;
    }
#endif
#endif

    IR1_OPND *opnd_mem = NULL;
    IR1_OPND *opnd_reg = NULL;
    if (ir1_opnd_is_mem(opnd0)) {
        opnd_mem = opnd0;
        opnd_reg = opnd1;
    } else {
        lsassert(ir1_opnd_is_mem(opnd1));
        opnd_mem = opnd1;
        opnd_reg = opnd0;
    }

    int opsz = ir1_opnd_size(opnd_mem);

    IR2_OPND src0_mem_addr = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    IR2_OPND label_atomic_helper = latxs_ir2_opnd_new_label();
    IR2_OPND label_atomic_exit = latxs_ir2_opnd_new_label();

    /* format the mem opnd */
    latxs_convert_mem_opnd_no_offset(&src0_mem_addr, opnd_mem);
    latxs_load_ir1_to_ir2(&src1, opnd_reg, EXMode_S);

#ifdef CONFIG_ATOMIC
#ifdef CONFIG_SOFTMMU
    bool is_load = false;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int mmu_index = td->sys.mem_index;
#endif
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    if (do_fast_atomic) {
        IR2_OPND label_xchg_align = latxs_ir2_opnd_new_label();
        IR2_OPND label_xchg_exit = latxs_ir2_opnd_new_label();
        /* src0_mem_addr aligned 32bit or 64bit */
        if (opsz == 64) {
            latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        } else if (opsz == 32) {
            latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        } else if (opsz == 16) {
            latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        }  else if (opsz == 8) {
            latxs_append_ir2_opnd3(ATO_LISA_AMSWAP(opsz), &tmp, &src0_mem_addr, &src1);
            latxs_store_ir2_to_ir1(&tmp, opnd_reg);
            latxs_ra_free_temp(&src0_mem_addr);
            latxs_ra_free_temp(&src1);
            return true;
        } else {
            lsassert(0);
        }
        latxs_append_ir2_opnd3(LISA_BEQ, &tmp, &latxs_zero_ir2_opnd, &label_xchg_align);

        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
        latxs_load_imm64_to_ir2(
            &latxs_arg3_ir2_opnd,
            (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

        latxs_tr_gen_call_to_helper((ADDR)atomis_xchg_functions[opsz2index(opsz)],
                atomis_xchg_functions_id[opsz2index(opsz)]);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

        latxs_store_ir2_to_ir1(&latxs_ret0_ir2_opnd, opnd_reg);

        latxs_append_ir2_opnd1(LISA_B, &label_xchg_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_xchg_align);
        latxs_append_ir2_opnd3(ATO_LISA_AMSWAP(opsz), &tmp, &src0_mem_addr, &src1);
        latxs_store_ir2_to_ir1(&tmp, opnd_reg);
        latxs_ra_free_temp(&src0_mem_addr);
        latxs_ra_free_temp(&src1);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_xchg_exit);
        return true;
    }
    /* src0_mem_addr aligned 32bit or 64bit */
    if (opsz == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x7);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 32) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x3);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    } else if (opsz == 16) {
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp, &src0_mem_addr, 0x1);
        latxs_append_ir2_opnd3(LISA_BNE, &tmp, &latxs_zero_ir2_opnd, &label_atomic_helper);
    }  else if (opsz == 8) {
        /* 8bit is always aligned */
        if (!(cpuinfo & CPUINFO_LAM_BH)) {
            latxs_append_ir2_opnd1(LISA_B, &label_atomic_helper);
        }
    } else {
        lsassert(0);
    }

    /*
     * 1. lookup QEMU TLB in native code
     *    and finish memory access if TLB hit
     *    temp register is free to use inside.
     */
    if (opsz == 64) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMSWAP_DB_D, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMSWAP_DB_D, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 32) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMSWAP_DB_W, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMSWAP_DB_W, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 16) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMSWAP_DB_H, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMSWAP_DB_H, &tmp, &src1, &src0_mem_addr);
#endif
    } else if (opsz == 8) {
#ifdef CONFIG_SOFTMMU
        tr_gen_atomic_lookup_qemu_tlb(LISA_AMSWAP_DB_B, &tmp, &src1, &src0_mem_addr, mmu_index,
                            is_load, &label_atomic_helper);
#else
        la_out3(LISA_AMSWAP_DB_B, &tmp, &src1, &src0_mem_addr);
#endif
    } else {
        lsassert(0);
    }

    latxs_store_ir2_to_ir1(&tmp, opnd_reg);

    /* 2. memory access finish. jump to exit */
    latxs_append_ir2_opnd1(LISA_B, &label_atomic_exit);
#endif
    /*
     * 3. slow path : call QEMU's helper
     */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_helper);

    latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }
    LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, src0_mem_addr);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg2_ir2_opnd, &src1);
    latxs_load_imm64_to_ir2(
        &latxs_arg3_ir2_opnd,
        (opsz2tcgmemop(opsz) << 4) | lsenv->tr_data->sys.mem_index);

    latxs_tr_gen_call_to_helper((ADDR)atomis_xchg_functions[opsz2index(opsz)],
            atomis_xchg_functions_id[opsz2index(opsz)]);
    latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);

    latxs_store_ir2_to_ir1(&latxs_ret0_ir2_opnd, opnd_reg);

    /* 4. exit from fast path */
    latxs_append_ir2_opnd1(LISA_LABEL, &label_atomic_exit);

    latxs_ra_free_temp(&src0_mem_addr);
    latxs_ra_free_temp(&src1);
    return true;
}
