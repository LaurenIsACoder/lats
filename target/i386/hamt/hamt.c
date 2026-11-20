/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/typedefs.h"
#include "qemu/xxhash.h"
#include "cpu.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "exec/ram_addr.h"

#include "dune-info.h"

#include "hamt.h"
#include "hamt-internal.h"
#include "hamt-options.h"
#include "hamt-tlb.h"
#include "hamt-ptw-internal.h"
#include "hamt-data.h"

#include "latx-native-asm.h"
#include "latx-options.h"
#include "latx-hack.h"
#include "latx-sys-mmio.h"
#include "latx-static-codes.h"

#include "tcg/tcg.h"
#include "tcg/tcg-ldst.h"

static void hamt_check_epc(CPUX86State *env, void *epc, const char *source)
{
    uint32_t *ptr = (void *)DMW_OUT((uint64_t)epc);
    if (!in_code_gen_buffer(ptr)) {
        fprintf(stderr,
                "\033[31m hamt excp not from code cache %p : %s\033[m\n",
                ptr, source);
    }
}

__thread bool hamt_cpu_running_state;
bool hamt_is_running(void)    { return hamt_cpu_running_state; }
void hamt_start_running(void) { hamt_cpu_running_state = true; }
void hamt_stop_running(void)  { hamt_cpu_running_state = false; }

extern uint64_t hamt_data_area[];
extern uint64_t hamt_tlb_code_area[];

static inline uint64_t hamt_get_mem_address(uint32_t *epc, int cpuid)
{
    uint32_t inst = *epc;
    uint32_t rj = (inst >> 5) & 0x1f;
    uint32_t opc = (inst >> 22) << 22;
    if (opc == OPC_VSTELM_D) {
        int si8 = (inst >> 10) & 0xff;
        int offset = (si8 << 24) >> 24;
        offset = offset << 3;
        uint64_t base = ((uint64_t*)(hamt_data_area[cpuid]))[rj];
        return base + offset;
    }
    int imm = (inst >> 10) & 0xfff;
    int simm = (imm << 20) >> 20;
    uint64_t base = ((uint64_t*)(hamt_data_area[cpuid]))[rj];
    return base + simm;
}

static inline uint32_t get_opc_from_epc(uint32_t *epc)
{
    uint32_t inst = *epc;
    uint32_t opc = (inst >> 22) << 22;
    return opc;
}

static inline int is_atomic_inst(uint32_t *epc)
{
    uint32_t inst = *epc;
    uint32_t opc = (inst >> 24) << 24;
    switch (opc) {
    case OPC_LL_W        :
    case OPC_SC_W        :
    case OPC_LL_D        :
    case OPC_SC_D        :
        return 1;
    default:
        break;
    }

    opc = (inst >> 15) << 15;

    switch (opc) {
    case OPC_SC_Q        :
    case OPC_AMCAS_B     :
    case OPC_AMCAS_H     :
    case OPC_AMCAS_W     :
    case OPC_AMCAS_D     :
    case OPC_AMCAS_DB_B  :
    case OPC_AMCAS_DB_H  :
    case OPC_AMCAS_DB_W  :
    case OPC_AMCAS_DB_D  :
    case OPC_AMSWAP_B    :
    case OPC_AMSWAP_H    :
    case OPC_AMADD_B     :
    case OPC_AMADD_H     :
    case OPC_AMSWAP_DB_B :
    case OPC_AMSWAP_DB_H :
    case OPC_AMADD_DB_B  :
    case OPC_AMADD_DB_H  :
    case OPC_AMSWAP_W    :
    case OPC_AMSWAP_D    :
    case OPC_AMADD_W     :
    case OPC_AMADD_D     :
    case OPC_AMAND_W     :
    case OPC_AMAND_D     :
    case OPC_AMOR_W      :
    case OPC_AMOR_D      :
    case OPC_AMXOR_W     :
    case OPC_AMXOR_D     :
    case OPC_AMMAX_W     :
    case OPC_AMMAX_D     :
    case OPC_AMMIN_W     :
    case OPC_AMMIN_D     :
    case OPC_AMMAX_WU    :
    case OPC_AMMAX_DU    :
    case OPC_AMMIN_WU    :
    case OPC_AMMIN_DU    :
    case OPC_AMSWAP_DB_W :
    case OPC_AMSWAP_DB_D :
    case OPC_AMADD_DB_W  :
    case OPC_AMADD_DB_D  :
    case OPC_AMAND_DB_W  :
    case OPC_AMAND_DB_D  :
    case OPC_AMOR_DB_W   :
    case OPC_AMOR_DB_D   :
    case OPC_AMXOR_DB_W  :
    case OPC_AMXOR_DB_D  :
    case OPC_AMMAX_DB_W  :
    case OPC_AMMAX_DB_D  :
    case OPC_AMMIN_DB_W  :
    case OPC_AMMIN_DB_D  :
    case OPC_AMMAX_DB_WU :
    case OPC_AMMAX_DB_DU :
    case OPC_AMMIN_DB_WU :
    case OPC_AMMIN_DB_DU :
        return 1;
    default:
        break;
    }
    return 0;
}

static inline int is_write_inst(uint32_t *epc)
{
    uint32_t inst = *epc;
    uint32_t opc = (inst >> 22) << 22;

    if (is_atomic_inst(epc)) return 1;

    switch(opc) {
        case OPC_LD_B:
        case OPC_LD_H:
        case OPC_LD_W:
        case OPC_LD_BU:
        case OPC_LD_HU:
        case OPC_LD_WU:
        case OPC_LD_D:
        case OPC_FLD_S:
        case OPC_FLD_D:
        case OPC_VLD:
            return 0;
        case OPC_ST_B:
        case OPC_ST_H:
        case OPC_ST_W:
        case OPC_ST_D:
        case OPC_FST_S:
        case OPC_FST_D:
        case OPC_VST:
        case OPC_VSTELM_D:
            return 1;
        default: {
            dune_info("inst: %x epc: %p", *epc, epc);
            dune_die("invalid opc in is_write_inst");
            return 0;
        }
    }
    return -1;
}

static MemOpIdx hamt_get_oi(uint32_t *epc, int mmu_idx)
{
    uint32_t inst = *epc;

    uint32_t opc = (inst >> 24) << 24;
    switch(opc) {
        case OPC_LL_W:
        case OPC_SC_W:
            return make_memop_idx(MO_LEUL, mmu_idx);
        case OPC_LL_D:
        case OPC_SC_D:
            return make_memop_idx(MO_LEUQ, mmu_idx);
        default: {
            break;
        }
    }

    opc = (inst >> 15) << 15;
    switch(opc) {
        case OPC_AMCAS_B:
        case OPC_AMSWAP_B:
        case OPC_AMADD_B:
        case OPC_AMCAS_DB_B:
        case OPC_AMSWAP_DB_B:
        case OPC_AMADD_DB_B:
            return make_memop_idx(MO_SB, mmu_idx);
        case OPC_AMCAS_H:
        case OPC_AMSWAP_H:
        case OPC_AMADD_H:
        case OPC_AMCAS_DB_H:
        case OPC_AMSWAP_DB_H:
        case OPC_AMADD_DB_H:
            return make_memop_idx(MO_LESW, mmu_idx);
        case OPC_AMCAS_W:
        case OPC_AMSWAP_W:
        case OPC_AMADD_W:
        case OPC_AMAND_W:
        case OPC_AMOR_W:
        case OPC_AMXOR_W:
        case OPC_AMCAS_DB_W:
        case OPC_AMSWAP_DB_W:
        case OPC_AMADD_DB_W:
        case OPC_AMAND_DB_W:
        case OPC_AMOR_DB_W:
        case OPC_AMXOR_DB_W:
            return make_memop_idx(MO_LESL, mmu_idx);
        case OPC_AMCAS_D:
        case OPC_AMSWAP_D:
        case OPC_AMADD_D:
        case OPC_AMAND_D:
        case OPC_AMOR_D:
        case OPC_AMXOR_D:
        case OPC_AMCAS_DB_D:
        case OPC_AMSWAP_DB_D:
        case OPC_AMADD_DB_D:
        case OPC_AMAND_DB_D:
        case OPC_AMOR_DB_D:
        case OPC_AMXOR_DB_D:
            return make_memop_idx(MO_LEUQ, mmu_idx);
        case OPC_SC_Q:
            return make_memop_idx(MO_128, mmu_idx);
        default: {
            break;
        }
    }

    opc = (inst >> 22) << 22;
    switch(opc) {
        case OPC_LD_B:
            return make_memop_idx(MO_SB, mmu_idx);
        case OPC_LD_BU:
        case OPC_ST_B:
            return make_memop_idx(MO_UB, mmu_idx);
        case OPC_LD_H:
            return make_memop_idx(MO_LESW, mmu_idx);
        case OPC_LD_HU:
        case OPC_ST_H:
            return make_memop_idx(MO_LEUW, mmu_idx);
        case OPC_LD_W:
            return make_memop_idx(MO_LESL, mmu_idx);
        case OPC_FLD_S:
        case OPC_FST_S:
        case OPC_LD_WU:
        case OPC_ST_W:
            return make_memop_idx(MO_LEUL, mmu_idx);
        case OPC_LD_D:
        case OPC_ST_D:
        case OPC_FLD_D:
        case OPC_FST_D:
        case OPC_VLD:
        case OPC_VST:
        case OPC_VSTELM_D:
            return make_memop_idx(MO_LEUQ, mmu_idx);
        default: {
            dune_info("inst: %x epc: %p", *epc, epc);
            dune_die("invalid opc in hamt_get_oi");
        }
    }
    return 0;
}

#ifdef TARGET_X86_64
#define ENV_FP_LO   0
#define ENV_FP_HI   31
#define ENV_VEC_MAP_LO  16
#define ENV_VEC_MAP_HI  31
#else /* target i386 */
#define ENV_FP_LO   0
#define ENV_FP_HI   23
#define ENV_VEC_MAP_LO  16
#define ENV_VEC_MAP_HI  23
#endif

#define ENV_FP_MAP_LO   0
#define ENV_FP_MAP_HI   7
#define ENV_FP_TMP_LO   9
#define ENV_FP_TMP_HI   15

static inline int is_env_fpr(int fp, int is_simd)
{
    /*
     * [ 0- 7] mapping fp registers
     * [ 9-15] temp fp registers
     * [16-23] mapping simd registers
     * [24-31] mapping simd registers for x64 guest
     */
    assert(ENV_FP_LO <= fp && fp <= ENV_FP_HI);
    return is_simd ? (ENV_VEC_MAP_LO <= fp && fp <= ENV_VEC_MAP_HI) :
                     (ENV_FP_MAP_LO  <= fp && fp <= ENV_FP_MAP_HI);
}

#define LB_MASK 0xffffffffffffff00
#define LH_MASK 0xffffffffffff0000
#define LW_MASK 0xffffffff00000000

static uint64_t hamt_get_st_val_softmmu(CPUX86State *env,
        uint32_t *epc, int cpuid)
{
    uint32_t inst = *epc;
    uint32_t opc = inst >> 22 << 22;
    uint32_t rd = inst & 0x1f; /* rd */

    switch(opc) {
    case OPC_ST_B:
        return ((uint64_t*)(hamt_data_area[cpuid]))[rd] & (uint64_t)0xff;
    case OPC_ST_H:
        return ((uint64_t*)(hamt_data_area[cpuid]))[rd] & (uint64_t)0xffff;
    case OPC_ST_W:
        return ((uint64_t*)(hamt_data_area[cpuid]))[rd] & (uint64_t)0xffffffff;
    case OPC_ST_D:
        return ((uint64_t*)(hamt_data_area[cpuid]))[rd];
    case OPC_FST_S:
        if (is_env_fpr(rd, 0)) {
            assert(option_lsfpu);
            assert(rd <= ENV_FP_MAP_HI);
            rd = (rd + env->fpstt) & 0x7;
#ifdef CONFIG_LATX_OPT_NOFP
            return __movfr2gr32_native_freg_temp((rd));
#else
            return env->fpregs[rd].d.low & (uint64_t)0xffffffff;
#endif
        } else if (is_env_fpr(rd, 1)) {
            assert(ENV_VEC_MAP_LO <= rd && rd <= ENV_VEC_MAP_HI);
#ifdef CONFIG_LATX_OPT_NOFP
            return __movfr2gr32_native_freg_temp((rd));
#else
            return env->xmm_regs[rd - ENV_VEC_MAP_LO]._l_ZMMReg[0];
#endif
        } else {
            assert(ENV_FP_TMP_LO <= rd && rd <= ENV_FP_TMP_HI);
            uint64_t fpdata = 0;
            __fst32_native_freg_temp(&fpdata, rd);
            return fpdata & (uint64_t)0xffffffff;
        }
    case OPC_FST_D:
        if (is_env_fpr(rd, 0)) {
            assert(option_lsfpu && rd <= ENV_FP_MAP_HI);
            rd = (rd + env->fpstt) & 0x7;
#ifdef CONFIG_LATX_OPT_NOFP
            return __movfr2gr64_native_freg_temp((rd));
#else
            return env->fpregs[rd].d.low;
#endif
        } else if (is_env_fpr(rd, 1)) {
            assert(ENV_VEC_MAP_LO <= rd && rd <= ENV_VEC_MAP_HI);
#ifdef CONFIG_LATX_OPT_NOFP
            return __movfr2gr64_native_freg_temp((rd));
#else
            return env->xmm_regs[rd - ENV_VEC_MAP_LO]._q_ZMMReg[0];
#endif
        } else {
            assert(ENV_FP_TMP_LO <= rd && rd <= ENV_FP_TMP_HI);
            uint64_t fpdata = 0;
            __fst64_native_freg_temp(&fpdata, rd);
            return fpdata; 
        }
       break;
    case OPC_VST:
       if (is_env_fpr(rd, 1)) {
           assert(ENV_VEC_MAP_LO <= rd && rd <= ENV_VEC_MAP_HI);
#ifdef CONFIG_LATX_OPT_NOFP
           __vst128_native_vreg_temp(&env->temp_xmm, (rd));
#else
           env->temp_xmm._q_ZMMReg[0] = env->xmm_regs[rd - ENV_VEC_MAP_LO]._q_ZMMReg[0];
           env->temp_xmm._q_ZMMReg[1] = env->xmm_regs[rd - ENV_VEC_MAP_LO]._q_ZMMReg[1];
#endif
           return 0;
       } else {
           assert(ENV_FP_TMP_LO <= rd && rd <= ENV_FP_TMP_HI);
           uint64_t vecdata[2];
           __vst128_native_vreg_temp(&(vecdata[0]), rd);
           env->temp_xmm._q_ZMMReg[0] = vecdata[0];
           env->temp_xmm._q_ZMMReg[1] = vecdata[1];
           return 0;
       }
       break;
    case OPC_VSTELM_D:
       if (is_env_fpr(rd, 1)) {
           assert(ENV_VEC_MAP_LO <= rd && rd <= ENV_VEC_MAP_HI);
#ifdef CONFIG_LATX_OPT_NOFP
           __vst128_native_vreg_temp(&env->temp_xmm, (rd));
#else
           env->temp_xmm._q_ZMMReg[0] = env->xmm_regs[rd - ENV_VEC_MAP_LO]._q_ZMMReg[0];
           env->temp_xmm._q_ZMMReg[1] = env->xmm_regs[rd - ENV_VEC_MAP_LO]._q_ZMMReg[1];
#endif
           return 0;
       } else {
           assert(ENV_FP_TMP_LO <= rd && rd <= ENV_FP_TMP_HI);
           uint64_t vecdata[2];
           __vst128_native_vreg_temp(&(vecdata[0]), rd);
           env->temp_xmm._q_ZMMReg[0] = vecdata[0];
           env->temp_xmm._q_ZMMReg[1] = vecdata[1];
           return 0;
       }
       break;
    default:
        dune_info("inst: %x epc: %p", *epc, epc);
       dune_die("invalid opc in hamt_get_st_val_softmmu");
       break;
    }

    dune_die("can not reach here in hamt_get_st_val_softmmu");
    return 0;
}

static void load_into_reg_softmmu(CPUX86State *env,
        uint64_t val, uint32_t *epc, int cpuid)
{
    uint32_t inst = *epc;
    int rt = inst & 0x1f; /* fd */
    uint32_t opc = inst >> 22 << 22;

    switch(opc) {
    case OPC_FLD_S:
        if (is_env_fpr(rt, 0)) {
            assert(option_lsfpu && ENV_FP_MAP_LO <= rt && rt <= ENV_FP_TMP_HI);
            rt = (rt + env->fpstt) & 0x7;
            *((uint32_t*)&env->fpregs[rt].d.low) = val & 0xffffffff;
#ifdef CONFIG_LATX_OPT_NOFP
            __movgr2fr32_native_freg_temp(val, rt);
#endif
        } else if (is_env_fpr(rt, 1)) {
            assert(ENV_VEC_MAP_LO <= rt && rt <= ENV_VEC_MAP_HI);
            env->xmm_regs[rt - ENV_VEC_MAP_LO]._l_ZMMReg[0] = val & 0xffffffff;
#ifdef CONFIG_LATX_OPT_NOFP
            __movgr2fr32_native_freg_temp(val, rt);
#endif
        } else {
            assert(ENV_FP_TMP_LO <= rt && rt <= ENV_FP_TMP_HI);
            __fld32_native_freg_temp(&val, rt);
        }
        return;
    case OPC_FLD_D:
        if (is_env_fpr(rt, 0)) {
            assert(option_lsfpu && ENV_FP_MAP_LO <= rt && rt <= ENV_FP_TMP_HI);
            rt = (rt + env->fpstt) & 0x7;
            *((uint64_t*)&env->fpregs[rt].d.low) = val;
#ifdef CONFIG_LATX_OPT_NOFP
            __movgr2fr64_native_freg_temp(val, rt);
#endif
        } else if (is_env_fpr(rt, 1)) {
            assert(ENV_VEC_MAP_LO <= rt && rt <= ENV_VEC_MAP_HI);
            env->xmm_regs[rt - ENV_VEC_MAP_LO]._q_ZMMReg[0] = val;
#ifdef CONFIG_LATX_OPT_NOFP
            __movgr2fr64_native_freg_temp(val, rt);
#endif
        } else {
            assert(ENV_FP_TMP_LO <= rt && rt <= ENV_FP_TMP_HI);
            __fld64_native_freg_temp(&val, rt);
        }
        return;
    case OPC_VLD:
        if (is_env_fpr(rt, 1)) {
            assert(ENV_VEC_MAP_LO <= rt && rt <= ENV_VEC_MAP_HI);
            env->xmm_regs[rt - ENV_VEC_MAP_LO]._q_ZMMReg[0] = env->temp_xmm._q_ZMMReg[0];
            env->xmm_regs[rt - ENV_VEC_MAP_LO]._q_ZMMReg[1] = env->temp_xmm._q_ZMMReg[1];
#ifdef CONFIG_LATX_OPT_NOFP
            __vld128_native_vreg_temp(&env->temp_xmm, rt);
#endif
        } else {
            assert(ENV_FP_TMP_LO <= rt && rt <= ENV_FP_TMP_HI);
            __vld128_native_vreg_temp(&env->temp_xmm, rt);
        }
        return;
    case OPC_LD_B: {
        uint64_t temp;
        temp = val & 0xff;
        temp |= (val & 0x80) ? LB_MASK : 0;
        val = temp;
        break;
    }
    case OPC_LD_H: {
        uint64_t temp;
        temp = val & 0xffff;
        temp |= (val & 0x8000) ? LH_MASK : 0;
        val = temp;
        break;
    }
    case OPC_LD_W: {
        uint64_t temp;
        temp = val & 0xffffffff;
        temp |= (val & 0x80000000) ? LW_MASK : 0;
        val = temp;
        break;
    }
    case OPC_LD_BU: val &= 0xff;       break;
    case OPC_LD_HU: val &= 0xffff;     break;
    case OPC_LD_WU: val &= 0xffffffff; break;
    case OPC_LD_D:                     break;
    default:
        dune_info("inst: %x epc: %p", *epc, epc);
        dune_die("invalid opc in load_into_reg_softmmu");
        break;
    }

    ((uint64_t *)(hamt_data_area[cpuid]))[rt] = val;
}




static inline void leave_lsfpu_mode(void) {
    __asm__ __volatile__ (X86CLRTM:::);
}
static inline void enter_lsfpu_mode(void) {
    __asm__ __volatile__ (X86SETTM:::);
}

static int lsfpu_get_ftop(void)
{
    int ftop = 0;
    __asm__ __volatile__ (
        X86MFTOP(%0)
        :"=r"(ftop)
        :
        :"memory", "t1"
        );
    return ftop;
}

static void lsfpu_set_ftop(int ftop)
{
    __asm__ __volatile__ (
        X86SETTM
        "andi       $t0,  %0,  0x7\n\r"    
        X86MTTOP0
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP1
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP2
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP3
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP4
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP5
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP6
        "beq        $t0, $zero, 1f\n\r"
        "addi.w     $t0, $t0, -1\n\r"
        X86MTTOP7
        "beq        $t0, $zero, 1f\n\r"
        "1:\n\r"
        :                                 
        :"r"(ftop)                         
        :"memory", "t0"             
        );                                
}

#ifndef CONFIG_LATX_OPT_EFLAGS
static void hamt_context_save_eflags(void *env_cc_src, void *env_cc_op)
{
    __asm__ __volatile__ (
        /* save eflags */
        __X86MFFLAG(12)
        "st.w       $t0,  %0,    0x0\n\r"
        "ori        $t0,  $zero, 0x1\n\r"
        "st.w       $t0,  %1,    0x0\n\r"
        :
        :"r"(env_cc_src), "r"(env_cc_op)
        :"memory", "t1", "t0"
        );
}

static void hamt_context_load_eflags(void *env_cc_src)
{
    __asm__ __volatile__ (                    
        "ld.w       $t0,  %0,  0x0\n\r"    
        __X86MTFLAG(12)
        :
        :"r"(env_cc_src)
        :"memory", "t1", "t0"
        );
}
#endif

static void hamt_save_from_native_gpr(CPUX86State *env, int cpuid)
{
    uint64_t *d = (void *)(hamt_data_area[cpuid]);
    env->regs[0] = d[24]; /* x86.RAX - LoongArch.S1 24 */
    env->regs[1] = d[25]; /* x86.RCX - LoongArch.S2 25 */
    env->regs[2] = d[26]; /* x86.RDX - LoongArch.S3 26 */
    env->regs[3] = d[27]; /* x86.RBX - LoongArch.S4 27 */
    env->regs[4] = d[28]; /* x86.RSP - LoongArch.S5 28 */
    env->regs[5] = d[29]; /* x86.RBP - LoongArch.S6 29 */
    env->regs[6] = d[30]; /* x86.RSI - LoongArch.S7 30 */
    env->regs[7] = d[31]; /* x86.RSI - LoongArch.S8 31 */

    env->regs[8]  = d[12]; /* x86.R9   - LoongArch.T0 12 */
    env->regs[9]  = d[13]; /* x86.R9   - LoongArch.T1 13 */
    env->regs[10] = d[14]; /* x86.R10  - LoongArch.T2 14 */
    env->regs[11] = d[15]; /* x86.R11  - LoongArch.T3 15 */
    env->regs[12] = d[16]; /* x86.R12  - LoongArch.T4 16 */
    env->regs[13] = d[17]; /* x86.R13  - LoongArch.T5 17 */
    env->regs[14] = d[18]; /* x86.R14  - LoongArch.T6 18 */
    env->regs[15] = d[19]; /* x86.R15  - LoongArch.T7 19 */
}

static void hamt_save_from_native(CPUX86State *env, int cpuid)
{
    hamt_save_from_native_gpr(env, cpuid);

    if (!option_soft_fpu) {
        env->fpstt = lsfpu_get_ftop();
        leave_lsfpu_mode();
    }

#ifndef CONFIG_LATX_OPT_NOFP
    __fst64_native_00(&(env->fpregs[0].d.low));
    __fst64_native_01(&(env->fpregs[1].d.low));
    __fst64_native_02(&(env->fpregs[2].d.low));
    __fst64_native_03(&(env->fpregs[3].d.low));
    __fst64_native_04(&(env->fpregs[4].d.low));
    __fst64_native_05(&(env->fpregs[5].d.low));
    __fst64_native_06(&(env->fpregs[6].d.low));
    __fst64_native_07(&(env->fpregs[7].d.low));

    __vst128_native_16(&(env->xmm_regs[0]._b_ZMMReg[0]));
    __vst128_native_17(&(env->xmm_regs[1]._b_ZMMReg[0]));
    __vst128_native_18(&(env->xmm_regs[2]._b_ZMMReg[0]));
    __vst128_native_19(&(env->xmm_regs[3]._b_ZMMReg[0]));
    __vst128_native_20(&(env->xmm_regs[4]._b_ZMMReg[0]));
    __vst128_native_21(&(env->xmm_regs[5]._b_ZMMReg[0]));
    __vst128_native_22(&(env->xmm_regs[6]._b_ZMMReg[0]));
    __vst128_native_23(&(env->xmm_regs[7]._b_ZMMReg[0]));
    __vst128_native_24(&(env->xmm_regs[8]._b_ZMMReg[0]));
    __vst128_native_25(&(env->xmm_regs[9]._b_ZMMReg[0]));
    __vst128_native_26(&(env->xmm_regs[10]._b_ZMMReg[0]));
    __vst128_native_27(&(env->xmm_regs[11]._b_ZMMReg[0]));
    __vst128_native_28(&(env->xmm_regs[12]._b_ZMMReg[0]));
    __vst128_native_29(&(env->xmm_regs[13]._b_ZMMReg[0]));
    __vst128_native_30(&(env->xmm_regs[14]._b_ZMMReg[0]));
    __vst128_native_31(&(env->xmm_regs[15]._b_ZMMReg[0]));
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    hamt_context_save_eflags(&(env->cc_src), &(env->cc_op));
#endif
}

static void hamt_restore_to_native(CPUX86State *env, int cpuid)
{
#ifndef CONFIG_LATX_OPT_EFLAGS
    hamt_context_load_eflags(&(env->cc_src));
#endif

    /* make sure not in lsfpu mode */
    if (!option_soft_fpu) leave_lsfpu_mode();
    if (!option_soft_fpu) lsfpu_set_ftop(env->fpstt);

#ifndef CONFIG_LATX_OPT_NOFP
    __fld64_native_00(&(env->fpregs[0].d.low));
    __fld64_native_01(&(env->fpregs[1].d.low));
    __fld64_native_02(&(env->fpregs[2].d.low));
    __fld64_native_03(&(env->fpregs[3].d.low));
    __fld64_native_04(&(env->fpregs[4].d.low));
    __fld64_native_05(&(env->fpregs[5].d.low));
    __fld64_native_06(&(env->fpregs[6].d.low));
    __fld64_native_07(&(env->fpregs[7].d.low));

    __vld128_native_16(&(env->xmm_regs[0]._b_ZMMReg[0]));
    __vld128_native_17(&(env->xmm_regs[1]._b_ZMMReg[0]));
    __vld128_native_18(&(env->xmm_regs[2]._b_ZMMReg[0]));
    __vld128_native_19(&(env->xmm_regs[3]._b_ZMMReg[0]));
    __vld128_native_20(&(env->xmm_regs[4]._b_ZMMReg[0]));
    __vld128_native_21(&(env->xmm_regs[5]._b_ZMMReg[0]));
    __vld128_native_22(&(env->xmm_regs[6]._b_ZMMReg[0]));
    __vld128_native_23(&(env->xmm_regs[7]._b_ZMMReg[0]));
    __vld128_native_24(&(env->xmm_regs[8]._b_ZMMReg[0]));
    __vld128_native_25(&(env->xmm_regs[9]._b_ZMMReg[0]));
    __vld128_native_26(&(env->xmm_regs[10]._b_ZMMReg[0]));
    __vld128_native_27(&(env->xmm_regs[11]._b_ZMMReg[0]));
    __vld128_native_28(&(env->xmm_regs[12]._b_ZMMReg[0]));
    __vld128_native_29(&(env->xmm_regs[13]._b_ZMMReg[0]));
    __vld128_native_30(&(env->xmm_regs[14]._b_ZMMReg[0]));
    __vld128_native_31(&(env->xmm_regs[15]._b_ZMMReg[0]));
#endif

    if (!option_soft_fpu) enter_lsfpu_mode();
}

#include "exec/ram_addr.h"

static int hamt_fill_hardware_tlb(CPUX86State *env,
        uint64_t addr, int mmu_idx, int is_write)
{
    uintptr_t index = tlb_index(env, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(env, mmu_idx, addr);
    uint64_t tlb_addr_w = tlb_read_idx(entry, MMU_DATA_STORE);
    uint64_t tlb_addr_r = tlb_read_idx(entry, MMU_DATA_LOAD);
    uint64_t paddr = addr + entry->addend;
    CPUTLBEntryFull *full = &env_tlb(env)->d[mmu_idx].fulltlb[index];
    int prot = 0;
    int is_tlb_notdirty = 0;

    int flags_w = tlb_addr_w & TLB_FLAGS_MASK;
    int flags_r = tlb_addr_r & TLB_FLAGS_MASK;
    flags_w |= full->slow_flags[MMU_DATA_STORE];
    flags_r |= full->slow_flags[MMU_DATA_LOAD];

    if (tlb_hit(tlb_addr_w, addr)) prot |= PROT_WRITE;
    if (tlb_hit(tlb_addr_r, addr)) prot |= PROT_READ;

    if (flags_r) prot &= ~PROT_READ;
    if (flags_w) prot &= ~PROT_WRITE;

    if (!prot) return 0;

#if 0
    int flags = flags_w | flags_r;
    printf("%s vaddr %p addend %p paddr %p " \
           "flags %x prot r%d w%d\n",
           __func__, (void *)addr, (void *)entry->addend, (void *)paddr,
           flags, !!(prot & PROT_READ), !!(prot & PROT_WRITE));
#endif

    int do_fill_tlb = 1;
    if (!hamt_do_vga() && paddr_in_vram(paddr)) {
        do_fill_tlb = 0;
    }
    if (do_fill_tlb) {
        hamt_set_hardware_tlb_nocheck(env, addr, paddr, prot, 0);
    }

    is_tlb_notdirty = (flags_w & TLB_NOTDIRTY) ? 1 : 0;
    return (do_fill_tlb << 16) | prot | (is_tlb_notdirty << 17);
}

#ifdef CONFIG_LATX_PRINT
static void hamt_tlb_handler_print_error_stat(CPUState *cpu,
        uint64_t vaddr, uint32_t *epc)
{
    CPUX86State *env = cpu->env_ptr;

    int cpuid = cpu->cpu_index;
    int cpl = (env->hflags & HF_CPL_MASK) >> HF_CPL_SHIFT;

    uint64_t estat = ((uint64_t *)(hamt_data_area[cpuid]))[PT_ESTAT_INDEX];
    switch ((estat >> 16) & 0x3f) {
    /* normal cases, redirect to softmmu helper and refill the tlb */
    case 0x1: /* tlb load */
    case 0x2: /* tlb store */
    case 0x4: /* tlb modified */
        break;

    /* unusual cases, print and go on */
    case 0x5: /* tlb not read */
        printf("%s CPU %d TLB Read Error : vaddr 0x%016lx epc %p cpl %d\n", __func__,
                cpuid, vaddr, epc, cpl);
        break;
    case 0x7: /* tlb privilege error */
        printf("%s CPU %d TLB PLV  Error : vaddr 0x%016lx epc %p cpl %d\n", __func__,
                cpuid, vaddr, epc, cpl);
        break;
    case 0x8: /* tlb address error */
        printf("%s CPU %d TLB ADE  Error : vaddr 0x%016lx epc %p cpl %d\n", __func__,
                cpuid, vaddr, epc, cpl);
        break;

    /* unexpected cases, stop here */
    case 0x3: /* tlb ifetch */
    case 0x6: /* tlb not exec */
    case 0x9: /* tlb unaligned address */
    default:
        assert(0);
        break;
    }
}
#endif

static inline void hamt_emu_ll_w(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    int rd = inst & 0x1f;
    int rj = ((inst >> 5) & 0x1f);
    int32_t val_rd = 0;
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    int imm14 = ((inst >> 10) & 0x3fff) << 2;
    imm14 = (imm14 << 16) >> 16; // signed extension
    uint64_t vaddr = val_rj + imm14;
    void *haddr = helper_mmu(env, vaddr, oi, oi_size, retaddr);
    __asm__ __volatile__ (
            "ll.w %0, %1, 0\r\n"
            :"=r"(val_rd):"r"(haddr):);
    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = (int64_t)val_rd;
}

static inline void hamt_emu_ll_d(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    int rd = inst & 0x1f;
    int rj = ((inst >> 5) & 0x1f);
    uint64_t val_rd = 0;
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    int imm14 = ((inst >> 10) & 0x3fff) << 2;
    imm14 = (imm14 << 16) >> 16; // signed extension
    uint64_t vaddr = val_rj + imm14;
    void *haddr = helper_mmu(env, vaddr, oi, oi_size, retaddr);
    __asm__ __volatile__ (
            "ll.d %0, %1, 0\r\n"
            :"=r"(val_rd):"r"(haddr):);

    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val_rd;
}

static inline void hamt_emu_sc_w(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    int rd = inst & 0x1f;
    int rj = ((inst >> 5) & 0x1f);
    uint64_t val_rd = ((uint64_t *)(hamt_data_area[cpuid]))[rd];
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    int imm14 = ((inst >> 10) & 0x3fff) << 2;
    imm14 = (imm14 << 16) >> 16; // signed extension
    uint64_t vaddr = val_rj + imm14;
    void *haddr = helper_mmu(env, vaddr, oi, oi_size, retaddr);
    uint64_t tmp = 0;
    __asm__ __volatile__ (
            "ll.w %2, %1, 0\r\n"
            "sc.w %0, %1, 0\r\n"
            :"=r"(val_rd):"r"(haddr),"r"(tmp):);
    (void)tmp;

    /* llbit which indicates if the store is performed */
    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val_rd;
}

static inline void hamt_emu_sc_d(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    int rd = inst & 0x1f;
    int rj = ((inst >> 5) & 0x1f);
    uint64_t val_rd = ((uint64_t *)(hamt_data_area[cpuid]))[rd];
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    int imm14 = ((inst >> 10) & 0x3fff) << 2;
    imm14 = (imm14 << 16) >> 16; // signed extension
    uint64_t vaddr = val_rj + imm14;
    void *haddr = helper_mmu(env, vaddr, oi, oi_size, retaddr);
    uint64_t tmp = 0;
    __asm__ __volatile__ (
            "ll.d %2, %1, 0\r\n"
            "sc.d %0, %1, 0\r\n"
            :"=r"(val_rd):"r"(haddr),"r"(tmp):);
    (void)tmp;

    /* llbit which indicates if the store is performed */
    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val_rd;
}

static inline uint64_t hamt_local_sc_q(uint64_t val_rd,
        uint64_t val_rk, uint64_t val_rj)
{
    uint64_t rd = val_rd;
    uint64_t rj = val_rj;
    uint64_t rk = val_rk;
    uint64_t tmp = 0;
    __asm__ __volatile__ (
            "ll.d %3, %2, 0\r\n"
            "sc.q %0, %1, %2\r\n"
            :
            : "r"(rd), "r"(rk), "r"(rj), "r"(tmp)
            :);
    (void)tmp;
    return rd; /* llbit which indicates if the store is performed */
}

static inline void hamt_emu_sc_q(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    uint32_t rd = inst & 0x1f;
    uint32_t rj = ((inst >> 5) & 0x1f);
    uint32_t rk = ((inst >> 10) & 0x1f);
    uint64_t val = 0;
    uint64_t val_rd = ((uint64_t *)(hamt_data_area[cpuid]))[rd];
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    uint64_t val_rk = ((uint64_t *)(hamt_data_area[cpuid]))[rk];
    void *haddr = helper_mmu(env, val_rj, oi, oi_size, retaddr);
    val = hamt_local_sc_q(val_rd, val_rk, (uint64_t)haddr);

    /* llbit which indicates if the store is performed */
    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val;
}

static bool hamt_ato_emu_llsc(CPUX86State *env,
        MemOpIdx oi, uint64_t retaddr, uint32_t inst, int cpuid,
        int emu_llsc)
{
    bool is_llsc = false;

    uint32_t opc = (inst >> 24) << 24;
    switch (opc) {
    case OPC_LL_W:
        if (emu_llsc) hamt_emu_ll_w(env, oi, retaddr, inst, cpuid);
        is_llsc = true;
        break;
    case OPC_LL_D:
        if (emu_llsc) hamt_emu_ll_d(env, oi, retaddr, inst, cpuid);
        is_llsc = true;
        break;
    case OPC_SC_W:
        if (emu_llsc) hamt_emu_sc_w(env, oi, retaddr, inst, cpuid);
        is_llsc = true;
        break;
    case OPC_SC_D:
        if (emu_llsc) hamt_emu_sc_d(env, oi, retaddr, inst, cpuid);
        is_llsc = true;
        break;
    default:
        break;
    }

    opc = (inst >> 15) << 15;
    switch (opc) {
    case OPC_SC_Q:
        if (emu_llsc) hamt_emu_sc_q(env, oi, retaddr, inst, cpuid);
        is_llsc = true;
    default:
        break;
    }

    return is_llsc;
}

static int hamt_tlb_handler_ato_emu(CPUX86State *env,
        uint64_t addr0, void *haddr0,
        MemOpIdx oi, uint32_t *epc, int cpuid, int emu_llsc)
{
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);

    uint64_t retaddr = ((uint64_t *)(hamt_data_area[cpuid]))[32];
    retaddr = DMW_OUT(retaddr) + 2;
    uint32_t inst = *(uint32_t *)epc;

    if (hamt_ato_emu_llsc(env, oi, retaddr, inst, cpuid, emu_llsc)) {
        return emu_llsc ? 0 : 1;
    }

    uint32_t rd = inst & 0x1f;
    uint32_t rj = ((inst >> 5) & 0x1f);
    uint32_t rk = ((inst >> 10) & 0x1f);
    uint64_t val = 0;

    uint64_t val_rd = ((uint64_t *)(hamt_data_area[cpuid]))[rd];
    uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
    uint64_t val_rk = ((uint64_t *)(hamt_data_area[cpuid]))[rk];

    void *haddr = haddr0;
    if (addr0 != val_rj) {
        /* atomic spans two pages */
        haddr = helper_mmu(env, val_rj, oi, oi_size, retaddr);
    }

    int8_t   *haddr8  = haddr; int8_t   val8;
    int16_t  *haddr16 = haddr; int16_t  val16;
    int32_t  *haddr32 = haddr; int32_t  val32;
    uint64_t *haddr64 = haddr; uint64_t val64;

    uint32_t opc = (inst >> 15) << 15;
    switch (opc) {
    case OPC_AMADD_B     :
    case OPC_AMADD_DB_B  : val = val8 = qatomic_fetch_add(haddr8, val_rk);
                           break;
    case OPC_AMADD_H     :
    case OPC_AMADD_DB_H  : val = val16 = qatomic_fetch_add(haddr16, val_rk);
                           break;
    case OPC_AMADD_W     :
    case OPC_AMADD_DB_W  : val = val32 = qatomic_fetch_add(haddr32, val_rk);
                           break;
    case OPC_AMADD_D     :
    case OPC_AMADD_DB_D  : val = val64 = qatomic_fetch_add(haddr64, val_rk);
                           break;
    case OPC_AMAND_W     :
    case OPC_AMAND_DB_W  : val = val32 = qatomic_fetch_and(haddr32, val_rk);
                           break;
    case OPC_AMAND_D     :
    case OPC_AMAND_DB_D  : val = val64 = qatomic_fetch_and(haddr64, val_rk);
                           break;
    case OPC_AMOR_W      :
    case OPC_AMOR_DB_W   : val = val32 = qatomic_fetch_or(haddr32, val_rk);
                           break;
    case OPC_AMOR_D      :
    case OPC_AMOR_DB_D   : val = val64 = qatomic_fetch_or(haddr64, val_rk);
                           break;
    case OPC_AMXOR_W     :
    case OPC_AMXOR_DB_W  : val = val32 = qatomic_fetch_xor(haddr32, val_rk);
                           break;
    case OPC_AMXOR_D     :
    case OPC_AMXOR_DB_D  : val = val64 = qatomic_fetch_xor(haddr64, val_rk);
                           break;
    case OPC_AMCAS_B     :
    case OPC_AMCAS_DB_B  : val = val8 = qatomic_cmpxchg__nocheck(haddr8, val_rd, val_rk);
                           break;
    case OPC_AMCAS_H     :
    case OPC_AMCAS_DB_H  : val = val16 = qatomic_cmpxchg__nocheck(haddr16, val_rd, val_rk);
                           break;
    case OPC_AMCAS_W     :
    case OPC_AMCAS_DB_W  : val = val32 = qatomic_cmpxchg__nocheck(haddr32, val_rd, val_rk);
                           break;
    case OPC_AMCAS_D     :
    case OPC_AMCAS_DB_D  : val = val64 = qatomic_cmpxchg__nocheck(haddr64, val_rd, val_rk);
                           break;
    case OPC_AMSWAP_B    :
    case OPC_AMSWAP_DB_B : val = val8 = qatomic_xchg__nocheck(haddr8, val_rk);
                           break;
    case OPC_AMSWAP_H    :
    case OPC_AMSWAP_DB_H : val = val16 = qatomic_xchg__nocheck(haddr16, val_rk);
                           break;
    case OPC_AMSWAP_W    :
    case OPC_AMSWAP_DB_W : val = val32 = qatomic_xchg__nocheck(haddr32, val_rk);
                           break;
    case OPC_AMSWAP_D    :
    case OPC_AMSWAP_DB_D : val = val64 = qatomic_xchg__nocheck(haddr64, val_rk);
                           break;
    default:
        dune_die("%s:%d atomic %x not handled yet\n", __func__, __LINE__, inst);
        break;
    }

    ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val;
    return 0;
}

static void hamt_tlb_handler_ato(uint64_t badv,
        CPUX86State *env, uint32_t *epc, int cpuid)
{
    hamt_save_from_native(env, cpuid);

    uint64_t addr = badv;

    int is_write = 1;
    int mmu_idx = cpu_mmu_index(env, false);
    MemOpIdx oi = hamt_get_oi(epc, mmu_idx);
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    uint64_t retaddr = ((uint64_t *)(hamt_data_area[cpuid]))[32];
    retaddr = DMW_OUT(retaddr) + 2;

    void *haddr = helper_mmu(env, addr, oi, oi_size, retaddr);
    int ret = hamt_fill_hardware_tlb(env, badv, mmu_idx, is_write);
#define RET_IS_RW(r)        (((r) & 0xff) == (PROT_WRITE | PROT_READ))
#define RET_IS_NOTDIRTY(r)  (((r) >> 17) & 0x1)
    /* if fail to fill TLB entry with proper prot */
    if (unlikely(!RET_IS_RW(ret))) {
        /* Emulate the atomic inst.
         *
         * LL/SC can not be perfectly emulated bacause
         * - The LL in code cache access memory with GVA
         * - At here, only HVA can be used after GVA -> GPA -> HVA
         * - SC with HVA will always fail
         */
        if (hamt_tlb_handler_ato_emu(env, addr, haddr, oi, epc, cpuid, 0)) {
            int do_emu_llsc = 1;
            if (RET_IS_NOTDIRTY(ret)) {
                /* If the page is protected by SMC, we could invalidate
                 * all TB on this page to make it writable again.
                 * Otherwise we can only risk to emulate the SC. */
#ifdef CONFIG_LATX_DEBUG
                printf("%s:%d invalidate page for ll/sc %lx %p %d epc %p\n",
                        __func__, __LINE__,
                        addr, haddr, oi_size, epc);
#endif
                helper_mmu_clear_smc_page(env, addr, oi, oi_size, retaddr);
                ret = hamt_fill_hardware_tlb(env, badv, mmu_idx, is_write);
                if (RET_IS_RW(ret)) do_emu_llsc = 0;
            }
            if (do_emu_llsc) {
#ifdef CONFIG_LATX_DEBUG
                printf("%s:%d risk to emulate ll/sc %lx %p %d epc %p %08x ret %x\n",
                        __func__, __LINE__,
                        addr, haddr, oi_size, epc, *epc, ret);
#endif
                hamt_tlb_handler_ato_emu(env, addr, haddr, oi, epc, cpuid, 1);
                ((uint64_t *)(hamt_data_area[cpuid]))[32] += 4;
            }
        } else {
            /* AM inst is emulated */
            ((uint64_t *)(hamt_data_area[cpuid]))[32] += 4;
        }
    }

    hamt_restore_to_native(env, cpuid);
}

int hamt_mmio_inst_type(uint32_t inst);

void hamt_mark_injected_ade(uint64_t pc, uint64_t rj, uint64_t rk);
void hamt_mark_injected_ade(uint64_t pc, uint64_t rj, uint64_t rk)
{
    CPUState *cpu = current_cpu;
    assert(cpu);

    CPUX86State *env = cpu->env_ptr;
    env->hamt_injected_ade = 1;
    env->hamt_injected_ade_pc = pc;
    env->hamt_injected_ade_rj = rj;
    env->hamt_injected_ade_rk = rk;
}

static void hamt_clear_injected_ade(void)
{
    CPUState *cpu = current_cpu;
    assert(cpu);

    CPUX86State *env = cpu->env_ptr;
    env->hamt_injected_ade = 0;
    env->hamt_injected_ade_pc = -1ULL;
}

static uint64_t hamt_fix_injected_ade(CPUX86State *env,
        uint32_t *epc, int cpuid)
{
    uint32_t inst = *epc;
    int rj = (inst >> 5) & 0x1f;
    int rk = (inst >> 10) & 0x1f;
    uint64_t *data = ((uint64_t *)(hamt_data_area[cpuid]));
    uint64_t base = 0;

    int imm = 0, simm = 0;
    int type = hamt_mmio_inst_type(inst);
    switch (type) {
    case 1: /* ld/st */
#ifdef CONFIG_LATX_DEBUG
        printf("%s:%d fix rj %lx to %lx\n", __func__, __LINE__,
                data[rj], env->hamt_injected_ade_rj);
#endif
        data[rj] = env->hamt_injected_ade_rj;
        imm = (inst >> 10) & 0xfff;
        simm = (imm << 20) >> 20;
        base = data[rj] + simm;
        break;
    case 2: /* ldx/stx */
#ifdef CONFIG_LATX_DEBUG
        printf("%s:%d fix rj %lx to %lx\n", __func__, __LINE__,
                data[rj], env->hamt_injected_ade_rj);
        printf("%s:%d fix rk %lx to %lx\n", __func__, __LINE__,
                data[rk], env->hamt_injected_ade_rk);
#endif
        data[rj] = env->hamt_injected_ade_rj;
        data[rk] = env->hamt_injected_ade_rk;
        rj = (inst >> 5) & 0x1f;
        rk = (inst >> 10) & 0x1f;
        base = data[rj] + data[rk];
        break;
    case 3: /* ldptr/stptr */
#ifdef CONFIG_LATX_DEBUG
        printf("%s:%d fix rj %lx to %lx\n", __func__, __LINE__,
                data[rj], env->hamt_injected_ade_rj);
#endif
        data[rj] = env->hamt_injected_ade_rj;
        imm = (inst >> 10) & 0x3fff;
        simm = (imm << 18) >> 18;
        base = data[rj] + simm;
        break;
    default:
        printf("%s:%d epc %p inst %08x\n",
                __func__, __LINE__, epc, inst);
        g_assert_not_reached();
        break;
    }
    hamt_save_from_native_gpr(env, cpuid);
    return base;
}

int hamt_mmio_inst_type(uint32_t inst)
{
    uint32_t opc = (inst >> 22) << 22;
    switch(opc) {
    case OPC_LD_B:
    case OPC_LD_H:
    case OPC_LD_W:
    case OPC_LD_BU:
    case OPC_LD_HU:
    case OPC_LD_WU:
    case OPC_ST_B:
    case OPC_ST_H:
    case OPC_ST_W:
    case OPC_ST_D:
        return 1;
    default:
        break;
    }
    opc = (inst >> 18) << 18;
    switch (opc) {
    case OPC_LDX_B:
    case OPC_LDX_H:
    case OPC_LDX_W:
    case OPC_LDX_BU:
    case OPC_LDX_HU:
    case OPC_LDX_WU:
    case OPC_STX_B:
    case OPC_STX_H:
    case OPC_STX_W:
    case OPC_STX_D:
        return 2;
    default:
        break;
    }
    opc = (inst >> 24) << 24;
    switch (opc) {
    case OPC_LDPTR_W:
    case OPC_LDPTR_D:
    case OPC_STPTR_W:
    case OPC_STPTR_D:
        return 3;
    default:
        break;
    }
    return 0;
}

static void hamt_tlb_handler_softmmu(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc)
{
    env = (void *)DMW_OUT(env);
    epc = (void *)DMW_OUT(epc);

    CPUState *cpu = env_cpu(env);
    int cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;

    if (is_atomic_inst(epc)) {
        assert(!(env->hamt_injected_ade));
        if (option_fast_atomic) {
            hamt_tlb_handler_ato(hamt_badvaddr, env, epc, cpuid);
        } else {
            ((uint64_t *)(hamt_data_area[cpuid]))[PT_ERA_INDEX] += 8;
        }
        return;
    }

    hamt_save_from_native(env, cpuid);

    uint64_t mem_addr = hamt_get_mem_address(epc, cpuid);
    if (mem_addr != hamt_badvaddr) {
        hamt_badvaddr = mem_addr;
    }

    int is_write = is_write_inst(epc) & 1;
    uint64_t addr = hamt_badvaddr;
    int mmu_idx = cpu_mmu_index(env, false);
    MemOpIdx oi = hamt_get_oi(epc, mmu_idx);
    uint64_t retaddr = ((uint64_t *)(hamt_data_area[cpuid]))[PT_ERA_INDEX];
    retaddr = DMW_OUT(retaddr);

#ifdef CONFIG_LATX_PRINT
    hamt_tlb_handler_print_error_stat(cpu, addr, epc);
#endif

    if (env->hamt_injected_ade) {
        uint64_t epc0 = (uint64_t)epc;
        uint64_t epc1 = (uint64_t)env->hamt_injected_ade_pc;
#ifdef CONFIG_LATX_DEBUG
        printf("%s:%d HAMT epc %lx\n", __func__, __LINE__, epc0);
        printf("%s:%d HAMT injected ade at %lx\n", __func__, __LINE__, epc1);
#endif
        if (epc0 == epc1) {
            uint64_t fix = hamt_fix_injected_ade(env, epc, cpuid);
            addr = hamt_badvaddr = fix;
        }
        else {
            printf("%s:%d error : inject ADE epc not equal\n" \
                    "epc hamt   %lx\n" \
                    "epc inject %lx\n" \
                    , __func__, __LINE__, epc0, epc1);
            printf("%s:%d CPU[%d] %p env %p\n", __func__, __LINE__,
                    cpu->cpu_index, cpu, env);
            while (1) ;
        }
        hamt_clear_injected_ade();
    }

    uint32_t opc = get_opc_from_epc(epc);

    uint64_t val = 0;
    if (is_write) {
        val = hamt_get_st_val_softmmu(env, epc, cpuid);
        switch(opc) {
        case OPC_ST_B: helper_stb_mmu(env, addr, val, oi, retaddr); break;
        case OPC_ST_H: helper_stw_mmu(env, addr, val, oi, retaddr);  break;
        case OPC_FST_S:
        case OPC_ST_W: helper_stl_mmu(env, addr, val, oi, retaddr);  break;
        case OPC_FST_D:
        case OPC_ST_D: helper_stq_mmu(env, addr, val, oi, retaddr);  break;
        /* @val is not used for VST, env->temp_xmm will be used */
        case OPC_VST: latxs_helper_stdq_mmu(env, addr, val, oi, retaddr); break;
        case OPC_VSTELM_D: latxs_helper_stdq_mmu_idx(env, addr, val, oi, retaddr, ((*epc)>>18)&1); break;
        default: assert(0); break;
        }
    } else {
        switch(opc) {
        case OPC_LD_B:  val = helper_ldsb_mmu(env, addr, oi, retaddr); break;
        case OPC_LD_BU: val = helper_ldub_mmu(env, addr, oi, retaddr); break;
        case OPC_LD_H:  val = helper_ldsw_mmu(env, addr, oi, retaddr);  break;
        case OPC_LD_HU: val = helper_lduw_mmu(env, addr, oi, retaddr);  break;
        case OPC_LD_W:  val = helper_ldsl_mmu(env, addr, oi, retaddr);  break;
        case OPC_FLD_S:
        case OPC_LD_WU: val = helper_ldul_mmu(env, addr, oi, retaddr);  break;
        case OPC_FLD_D:
        case OPC_LD_D:  val = helper_ldq_mmu(env, addr, oi, retaddr);   break;
        /* no return for VLD, env->temp_xmm will be used */
        case OPC_VLD:  latxs_helper_lddq_mmu(env, addr, oi, retaddr);   break;
        default: assert(0); break;
        }
        load_into_reg_softmmu(env, val, epc, cpuid);
    }

    ((uint64_t *)(hamt_data_area[cpuid]))[PT_ERA_INDEX] += 4;

    hamt_fill_hardware_tlb(env, hamt_badvaddr, mmu_idx, is_write);

    hamt_restore_to_native(env, cpuid);
}

#ifdef CONFIG_LATX_HACK_MEM
static void hamt_recover_ptn_eflag(CPUX86State *env, uintptr_t epc);
#endif

void cpu_loop_exit_recover_ptn_eflag(CPUState *cpu, uintptr_t epc)
{
    CPUX86State *env = cpu->env_ptr;
    TranslationBlock *tb = NULL;

    if (!flag_reduction_tu_need_recover()) { return; }
    tb = tcg_tb_lookup(epc);
    if (!tb) { return; }
    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_INVALID) { return; }
    uint64_t ptn_start_pc = tb->pc + tb->ptn.pcoffset;
    if (env->eip <= ptn_start_pc) { return; }

#ifdef CONFIG_LATX_HACK_MEM
    if (env->in_hamt_exception) {
        hamt_recover_ptn_eflag(env, epc);
        return;
    }
#endif

    fprintf(stderr, "\033[31m tb %p eflag not recover.\033[m\n", tb);
}

#ifdef CONFIG_LATX_HACK_MEM

static void hamt_recover_ptn_eflag(CPUX86State *env, uintptr_t epc)
{
    CPUState *cpu = env_cpu(env);
    int cpuid = cpu->cpu_index;
    uint64_t *d = (void *)(hamt_data_area[cpuid]);
    TranslationBlock *tb = tcg_tb_lookup(epc);

    if (!flag_reduction_tu_need_recover()) { return; }
    if (!tb) { return; }
    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_INVALID) { return; }
    uint64_t ptn_start_pc = tb->pc + tb->ptn.pcoffset;
    if (env->eip <= ptn_start_pc) { return; }

    int a0 = tb->ptn.opnd0;
    int a1 = tb->ptn.opnd1;
    uint64_t d0 = d[a0];
    uint64_t d1 = d[a1];
    int os = tb->ptn.os;

//    for (int i = 0; i < 32; ++i) { printf("%s:%d context reg[%d] = %lx\n", __func__, __LINE__, i, d[i]); }
    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_X86MTFLAG) {
        __asm__ __volatile__(
                "or $t0, $zero, %[opnd0]\n\r"
                ".word 0x005C0020 | (1<<10) | 12\n\r"
                :
                : [opnd0] "r" (d0)
                : "t0");
    }

    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_X86SUB) {
//        printf("%s:%d tb %p pc %lx epc %lx os %d opnd [%d]%ld [%d]%ld\n", __func__, __LINE__, tb, tb->pc, epc, os, a0, d0, a1, d1);
        if (os == 8) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F0008 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 16) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F0009 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 32) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F000A | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F000B | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        }
    }

    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_X86SRL) {
        if (os == 8) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F0018 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0), [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 16) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F0019 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0), [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 32) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F001A | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0), [opnd1] "r" (d1)
                    : "t0", "t1");
        } else {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F001B | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0), [opnd1] "r" (d1)
                    : "t0", "t1");
        }
    }

    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_X86AND) {
//        printf("%s:%d tb %p pc %lx epc %lx os %d opnd [%d]%ld [%d]%ld\n", __func__, __LINE__, tb, tb->pc, epc, os, a0, d0, a1, d1);
        if (os == 8) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8010 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 16) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8011 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 32) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8012 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8013 | (12<<5) | (13<<10) \n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        }
    }

    if (tb->ptn.eflag_type == PTN_EFLAG_TYPE_X86OR) {
        if (os == 8) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8014 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 16) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8015 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else if (os == 32) {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8016 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        } else {
            __asm__ __volatile__(
                    "or $t0, $zero, %[opnd0]\n\r"
                    "or $t1, $zero, %[opnd1]\n\r"
                    ".word  0x003F8017 | (12<<5) | (13<<10)\n\r"
                    :
                    : [opnd0] "r" (d0),  [opnd1] "r" (d1)
                    : "t0", "t1");
        }
    }
}

void raise_exception_err_ra(CPUX86State *env, int exception_index,
        int error_code, uintptr_t retaddr);
void lats_do_interrupt64(CPUX86State *env, int intno, int is_int,
        int error_code, target_ulong next_eip, int is_hw);

static void hamt_tlb_handler_page_fault(CPUX86State *env,
        uint64_t addr, int error_code, uintptr_t epc)
{
    assert(get_pg_mode(env) == 0x1000f);

    CPUState *cpu = env_cpu(env);
    int cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;
    hamt_save_from_native(env, cpuid);
#ifdef CONFIG_LATX_OPT_NOFP
    /* save guest context before going to guest exception */
    __nofp_sync_xmm_to_env(cpu);
    __nofp_sync_fpr_to_env(cpu, 0);
#endif

    /* set the bad address of memory access */
    env->cr[2] = addr;

    int64_t sext = (int64_t)addr >> 47;
    if (sext != 0 && sext != -1) {
        if (env->hamt_injected_ade) {
            uint64_t epc0 = (uint64_t)epc;
            uint64_t epc1 = (uint64_t)env->hamt_injected_ade_pc;
#ifdef CONFIG_LATX_DEBUG
            printf("%s:%d HAMT epc %lx\n", __func__, __LINE__, epc0);
            printf("%s:%d HAMT injected ade at %lx\n", __func__, __LINE__, epc1);
#endif
            if (epc0 == epc1) {
                uint64_t fix = hamt_fix_injected_ade(env, (void *)epc, cpuid);
                env->cr[2] = addr = fix;
            } else {
                printf("%s:%d error : inject ADE epc not equal\n" \
                        "epc hamt   %lx\n" \
                        "epc inject %lx\n" \
                        , __func__, __LINE__, epc0, epc1);
                printf("%s:%d CPU[%d] %p env %p\n", __func__, __LINE__,
                        cpu->cpu_index, cpu, env);
                while (1) ;
            }
            hamt_clear_injected_ade();
        }
        /* Recover deferred TU flags before leaving the HAMT exception. */
        cpu_restore_state(cpu, (uintptr_t)epc);
        hamt_recover_ptn_eflag(env, epc);
        raise_exception_err_ra(env, EXCP0D_GPF, 0, 0);
        g_assert_not_reached();
    }

//    raise_exception_err_ra(env, EXCP0E_PAGE, error_code, epc);
//    g_assert_not_reached();

    cpu_restore_state(cpu, (uintptr_t)epc);
    hamt_recover_ptn_eflag(env, epc);

    lats_do_interrupt64(env, EXCP0E_PAGE, 0, error_code, 0, 0);
    /* RIP and RSP is updated */

    uint64_t intbtblookup = GET_SC_TABLE(0, intb_lookup);
    intbtblookup = DMW_IN(intbtblookup);
    ((uint64_t *)(hamt_data_area[cpuid]))[28] = env->regs[R_ESP];
    ((uint64_t *)(hamt_data_area[cpuid]))[PT_ERA_INDEX] = intbtblookup;
    ((uint64_t *)(hamt_data_area[cpuid]))[5] = env->eip;
    hamt_restore_to_native(env, cpuid);
}

#endif

void hamt_tlb_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc)
{
    hamt_check_epc(env, epc, __func__);

    env = (void *)DMW_OUT((uint64_t)env);
    epc = (void *)DMW_OUT((uint64_t)epc);

    env->in_hamt_exception = true;
#ifdef CONFIG_LATX_HACK_MEM
    hamt_ptw_error_t ptwerror;
    ptwerror.error = HAMT_PTW_ERROR_NONE;
    ptwerror.paddr = 0;
    if (hamt_try_do_ptw(env,
                        hamt_badvaddr,
                        epc,
                        is_write_inst(epc),
                        &ptwerror))
    {
        env->in_hamt_exception = false;
        return;
    }

    if (option_opt_mmio && ptwerror.error == HAMT_PTW_ERROR_MMIO) {
        if (option_opt_mmio_apic() && latx_mem_is_apic(ptwerror.paddr)) {
#ifdef LATX_SYS_DEBUG_OPT_MMIO
            printf("%s:%d mmio on apic : addr %016lx paddr %016lx\n",
                    __func__, __LINE__, hamt_badvaddr, ptwerror.paddr);
#endif
            latx_mem_set_apic_vpage(hamt_badvaddr);
            latx_do_mmio_tb_retranslate(epc, TB_OPT_MMIO_APIC);
        }
        if (option_opt_mmio_hpet() && latx_mem_is_hpet(ptwerror.paddr)) {
#ifdef LATX_SYS_DEBUG_OPT_MMIO
            printf("%s:%d mmio on hpet : addr %016lx paddr %016lx\n",
                    __func__, __LINE__, hamt_badvaddr, ptwerror.paddr);
#endif
            latx_mem_set_hpet_vpage(hamt_badvaddr);
            latx_do_mmio_tb_retranslate(epc, TB_OPT_MMIO_HPET);
        }
    }

#ifdef CONFIG_LATX_PROFILER
    TranslationBlock *tb = tcg_tb_lookup((uintptr_t )epc);
    if (tb) {
        tb->profile.hamt_error[ptwerror.error] += 1;
    }
#endif

    if (option_fast_excep && ptwerror.error == HAMT_PTW_ERROR_PF) {
        hamt_tlb_handler_page_fault(env, hamt_badvaddr,
                                    ptwerror.error_code,
                                    (uintptr_t)epc);
        env->in_hamt_exception = false;
        return;
    }

#endif

    hamt_tlb_handler_softmmu(hamt_badvaddr, env, epc);
    env->in_hamt_exception = false;
}

extern uint64_t hamt_fpe_handler_code_area[HAMT_CPU_MAX];

void hamt_fpe_handler(CPUX86State *env, uint32_t *epc, uint32_t fcsr)
{
    hamt_check_epc(env, epc, __func__);

    if (!option_sigfpe) return;

    env = (void *)DMW_OUT(env);
    epc = (void *)DMW_OUT(epc);

    CPUState *cpu = env_cpu(env);
    int cpuid = cpu->cpu_index;

    hamt_save_from_native(env, cpuid);

    uint32_t fcsr0 = fcsr & 0xffffffe0;
    __write_native_fcsr0(fcsr0);

    uint32_t mxcsr_mask = (env->mxcsr >> 7) & 0x3f;
    uint32_t inst = *(uint32_t *)epc;
    uint32_t rd = inst & 0x1f; (void)rd;
    uint32_t rj = ((inst >> 5) & 0x1f); (void)rj;
    uint32_t rk = ((inst >> 10) & 0x1f); (void)rk;

    if (!option_fast_masked_v) goto fpereturnnext;
    if (!(mxcsr_mask == 0x3f)) goto fpereturnnext;

#ifdef CONFIG_LATX_OPT_NOFP
    uint32_t data32[4];
    uint64_t data64[2];

    uint32_t *p = (uint32_t *)hamt_fpe_handler_code_area[cpuid];
    p = (void *)DMW_OUT(p);
    int i = 0;
    p[i++] = inst;
    /* jirl zero, ra, 0 */
    p[i++] = 0x4c000020;
    asm volatile ("\tibar 0\n"::);

    void (*func)(void) = (void *)p;
    func();

    switch(inst >> 10) {
    case 0x46c1:    /* ftint.w.s */
    case 0x46c2:    /* ftint.w.d */
    case 0x46a1:    /* ftintrz.w.s */
    case 0x46a2:    /* ftintrz.w.d */
        data32[0] = __movfr2gr32_native_freg_temp(rd);
        if (data32[0] == 0x7fffffff) {
            __movgr2fr32_native_freg_temp(0x80000000, rd);
        }
        break;
    case 0x46c9:    /* ftint.l.s */
    case 0x46ca:    /* ftint.l.d */
    case 0x46a9:    /* ftintrz.l.s */
    case 0x46aa:    /* ftintrz.l.d */
        data64[0] = __movfr2gr64_native_freg_temp(rd);
        if (data64[0] == 0x7fffffffffffffff) {
            __movgr2fr64_native_freg_temp(0x8000000000000000, rd);
        }
        break;
    case 0x1ca78c:    /* vftint.w.s */
    case 0x1ca792:    /* vftintrz.w.s */
        __vst128_native_vreg_temp(data32, rd);
        for (int i = 0; i < 4; i++) {
            if (data32[i] == 0x7fffffff)
                data32[i] = 0x80000000;
        }
        __vld128_native_vreg_temp(data32, rd);
        break;
    default:
        break;
    }

    switch (inst >> 15) {
    case 0xe293:    /* vftint.w.d */
    case 0xe296:    /* vftintrz.w.d */
        __vst128_native_vreg_temp(data32, rd);
        for (int i = 0; i < 4; i++) {
            if (data32[i] == 0x7fffffff)
                data32[i] = 0x80000000;
        }
        __vld128_native_vreg_temp(data32, rd);
    default:
        break;
    }

    p[i--] = 0x0;
    p[i--] = 0x0;

#else
    fprintf(stderr, "in %s:%d but latx nofp disabled.\n", __func__, __LINE__);
#endif

fpereturnnext:
    if (mxcsr_mask == 0x3f) {

    } else if (mxcsr_mask == 0x3e) {
        fcsr |= 0x100000;
        env->excp_divz = 1;
    } else {
        fcsr &= 0xffffff;
        env->excp_divz = 1;
    }
    __write_native_fcsr0(fcsr);
    ((uint64_t *)(hamt_data_area[cpuid]))[32] += 4;

    if (option_lazy_mxcsr) {
        if(fcsr & 0x1) {    /* PE */
            env->mxcsr |= 0x20;
        }
        if(fcsr & 0x2) {    /* UE */
            env->mxcsr |= 0x10;
        }
        if(fcsr & 0x4) {    /* OE */
            env->mxcsr |= 0x8;
        }
        if(fcsr & 0x8) {    /* ZE */
            env->mxcsr |= 0x4;
        }
        if(fcsr & 0x10) {    /* IE */
            env->mxcsr |= 0x1;
        }
    }

    hamt_restore_to_native(env, cpuid);
    return;
}

void hamt_ale_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc)
{
    hamt_check_epc(env, epc, __func__);

    if ((!option_fast_atomic)) return;

    CPUState *cpu = env_cpu(env);
    int cpuid = cpu->cpu_index;
    env->in_hamt_exception = true;

    hamt_save_from_native(env, cpuid);

    uint32_t inst = *(uint32_t *)epc;
    int mmu_idx = cpu_mmu_index(env, false);
    MemOpIdx oi = hamt_get_oi(epc, mmu_idx);
    int oi_size = 1 << ((oi >> 4) & MO_SIZE);
    uint64_t retaddr = ((uint64_t *)(hamt_data_area[cpuid]))[32];
    retaddr = DMW_OUT(retaddr);

    uint32_t opc = (inst >> 24) << 24;
    uint32_t rd = inst & 0x1f;
    uint32_t rj = ((inst >> 5) & 0x1f);
    /* illegal in ll/sc */
    uint32_t rk = ((inst >> 10) & 0x1f);

    switch (opc) {
    case OPC_LL_W        :
    case OPC_SC_W        :
    case OPC_LL_D        :
    case OPC_SC_D        :
        /* May trigger a page fault and siglongjmp from cpu_loop_exit. */
        helper_mmu(env, hamt_badvaddr, oi, oi_size, retaddr + 2);
        dune_die("helper_mmu shall not return ll/sc %x oi %x badv %lx rj %lx",
                inst, oi, hamt_badvaddr,
                ((uint64_t *)(hamt_data_area[cpuid]))[rj]);
        break;
    default:
        break;
    }

    opc = (inst >> 15) << 15;
    switch (opc) {
    case OPC_SC_Q        :
    case OPC_AMCAS_H     :
    case OPC_AMCAS_W     :
    case OPC_AMCAS_D     :
    case OPC_AMCAS_DB_H  :
    case OPC_AMCAS_DB_W  :
    case OPC_AMCAS_DB_D  :
    case OPC_AMADD_H     :
    case OPC_AMADD_DB_H  :
    case OPC_AMADD_W     :
    case OPC_AMADD_D     :
    case OPC_AMAND_W     :
    case OPC_AMAND_D     :
    case OPC_AMOR_W      :
    case OPC_AMOR_D      :
    case OPC_AMXOR_W     :
    case OPC_AMXOR_D     :
    case OPC_AMMAX_W     :
    case OPC_AMMAX_D     :
    case OPC_AMMIN_W     :
    case OPC_AMMIN_D     :
    case OPC_AMMAX_WU    :
    case OPC_AMMAX_DU    :
    case OPC_AMMIN_WU    :
    case OPC_AMMIN_DU    :
    case OPC_AMADD_DB_W  :
    case OPC_AMADD_DB_D  :
    case OPC_AMAND_DB_W  :
    case OPC_AMAND_DB_D  :
    case OPC_AMOR_DB_W   :
    case OPC_AMOR_DB_D   :
    case OPC_AMXOR_DB_W  :
    case OPC_AMXOR_DB_D  :
    case OPC_AMMAX_DB_W  :
    case OPC_AMMAX_DB_D  :
    case OPC_AMMIN_DB_W  :
    case OPC_AMMIN_DB_D  :
    case OPC_AMMAX_DB_WU :
    case OPC_AMMAX_DB_DU :
    case OPC_AMMIN_DB_WU :
    case OPC_AMMIN_DB_DU :
        //printf("ALE atomic insn %x not handled!\n", inst);
        helper_mmu(env, hamt_badvaddr, oi, oi_size, retaddr + 2);
        dune_die("helper_mmu shall not return am %x oi %x badv %lx rj %lx",
                inst, oi, hamt_badvaddr,
                ((uint64_t *)(hamt_data_area[cpuid]))[rj]);
        break;
    case OPC_AMSWAP_H    :
    case OPC_AMSWAP_DB_H :
    {
        uint32_t val = 0;
        uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
        uint64_t val_rk = ((uint64_t *)(hamt_data_area[cpuid]))[rk];
        //val = helper_atomic_xchgw_le(env, val_rj, val_rk, oi);
        val = cpu_atomic_xchgw_le_mmu(env, val_rj, val_rk, oi, retaddr + 2);
        ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val;
        break;
    }
    case OPC_AMSWAP_W    :
    case OPC_AMSWAP_DB_W :
    {
        uint32_t val = 0;
        uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
        uint64_t val_rk = ((uint64_t *)(hamt_data_area[cpuid]))[rk];
        //val = helper_atomic_xchgl_le(env, val_rj, val_rk, oi);
        val = cpu_atomic_xchgl_le_mmu(env, val_rj, val_rk, oi, retaddr + 2);
        ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val;
        break;
    }
    case OPC_AMSWAP_D    :
    case OPC_AMSWAP_DB_D :
    {
        uint64_t val = 0;
        uint64_t val_rj = ((uint64_t *)(hamt_data_area[cpuid]))[rj];
        uint64_t val_rk = ((uint64_t *)(hamt_data_area[cpuid]))[rk];
        //val = helper_atomic_xchgq_le(env, val_rj, val_rk, oi);
        val = cpu_atomic_xchgq_le_mmu(env, val_rj, val_rk, oi, retaddr + 2);
        ((uint64_t *)(hamt_data_area[cpuid]))[rd] = val;
        break;
    }
    case OPC_AMCAS_B     :
    case OPC_AMADD_B     :
    case OPC_AMSWAP_B    :
    case OPC_AMCAS_DB_B  :
    case OPC_AMADD_DB_B  :
    case OPC_AMSWAP_DB_B :
    default:
        break;
    }

    ((uint64_t *)(hamt_data_area[cpuid]))[32] += 4;

    hamt_restore_to_native(env, cpuid);
    env->in_hamt_exception = false;

    return;
}

void loongarch_irq_enable(void)
{
    uint32_t flags = CSR_CRMD_IE;
    __asm__ __volatile__(
        "csrxchg %[val], %[mask], %[reg]\n\t"
        : [val] "+r" (flags)
        : [mask] "r" (CSR_CRMD_IE), [reg] "i" (LOONGARCH_CSR_CRMD)
        : "memory");
}

void restore_prmd(int cpuid)
{
    uint64_t prmd = ((uint64_t *)(hamt_data_area[cpuid]))[PT_PRMD_INDEX];
    write_csr_prmd(prmd);
}

void loongarch_ertn(void)
{
   __asm__ volatile (
        "csrwr %0, 0x6\n\t"
        "ertn\n"
        : : "r"(&&end)
        : "memory"
    );
end:
    return;
}
