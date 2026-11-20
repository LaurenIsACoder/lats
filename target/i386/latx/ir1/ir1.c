/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "ir1.h"

IR1_OPND al_ir1_opnd;
IR1_OPND ah_ir1_opnd;
IR1_OPND ax_ir1_opnd;
IR1_OPND eax_ir1_opnd;
IR1_OPND rax_ir1_opnd;

IR1_OPND cl_ir1_opnd;
IR1_OPND ch_ir1_opnd;
IR1_OPND cx_ir1_opnd;
IR1_OPND ecx_ir1_opnd;
IR1_OPND rcx_ir1_opnd;

IR1_OPND dl_ir1_opnd;
IR1_OPND dh_ir1_opnd;
IR1_OPND dx_ir1_opnd;
IR1_OPND edx_ir1_opnd;
IR1_OPND rdx_ir1_opnd;

IR1_OPND bl_ir1_opnd;
IR1_OPND bh_ir1_opnd;
IR1_OPND bx_ir1_opnd;
IR1_OPND ebx_ir1_opnd;
IR1_OPND rbx_ir1_opnd;

IR1_OPND sp_ir1_opnd;
IR1_OPND esp_ir1_opnd;
IR1_OPND rsp_ir1_opnd;

IR1_OPND bp_ir1_opnd;
IR1_OPND ebp_ir1_opnd;
IR1_OPND rbp_ir1_opnd;

IR1_OPND si_ir1_opnd;
IR1_OPND esi_ir1_opnd;
IR1_OPND rsi_ir1_opnd;

IR1_OPND di_ir1_opnd;
IR1_OPND edi_ir1_opnd;
IR1_OPND rdi_ir1_opnd;

IR1_OPND eax_mem8_ir1_opnd;
IR1_OPND ecx_mem8_ir1_opnd;
IR1_OPND edx_mem8_ir1_opnd;
IR1_OPND ebx_mem8_ir1_opnd;
IR1_OPND esp_mem8_ir1_opnd;
IR1_OPND ebp_mem8_ir1_opnd;
IR1_OPND esi_mem8_ir1_opnd;
IR1_OPND edi_mem8_ir1_opnd;

IR1_OPND eax_mem16_ir1_opnd;
IR1_OPND ecx_mem16_ir1_opnd;
IR1_OPND edx_mem16_ir1_opnd;
IR1_OPND ebx_mem16_ir1_opnd;
IR1_OPND esp_mem16_ir1_opnd;
IR1_OPND ebp_mem16_ir1_opnd;
IR1_OPND esi_mem16_ir1_opnd;
IR1_OPND edi_mem16_ir1_opnd;

IR1_OPND eax_mem32_ir1_opnd;
IR1_OPND ecx_mem32_ir1_opnd;
IR1_OPND edx_mem32_ir1_opnd;
IR1_OPND ebx_mem32_ir1_opnd;
IR1_OPND esp_mem32_ir1_opnd;
IR1_OPND ebp_mem32_ir1_opnd;
IR1_OPND esi_mem32_ir1_opnd;
IR1_OPND edi_mem32_ir1_opnd;

__thread csh handle;
#ifdef CONFIG_SOFTMMU
__thread csh handle16;
#endif
__thread csh handle64;

static inline __attribute__ ((always_inline))
IR1_OPND ir1_opnd_new_static_reg(IR1_OPND_TYPE opnd_type, int size,
                                        x86_reg reg)
{
    IR1_OPND ir1_opnd;
    IR1_OPND *opnd = &ir1_opnd;

    opnd->type = opnd_type;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->reg = reg;
    opnd->avx_bcast = X86_AVX_BCAST_INVALID;

    return ir1_opnd;
}

static inline __attribute__ ((always_inline))
IR1_OPND ir1_opnd_new_static_mem(IR1_OPND_TYPE opnd_type, int size,
                                        x86_reg reg, uint64_t imm)
{
    switch (reg) {
    case X86_REG_EAX:
    case X86_REG_ECX:
    case X86_REG_EBX:
    case X86_REG_EDX:
    case X86_REG_ESI:
    case X86_REG_EDI:
    case X86_REG_ESP:
    case X86_REG_EBP:
        break;
    default:
        lsassert(0);
    }

    IR1_OPND ir1_opnd;
    IR1_OPND *opnd = &ir1_opnd;

    opnd->type = opnd_type;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->size = size / 8;
    opnd->mem.base = reg;

    opnd->mem.index = X86_REG_INVALID;
    opnd->mem.segment = X86_REG_INVALID;
    opnd->mem.scale = 0;
    opnd->mem.disp = imm;

    opnd->avx_bcast = X86_AVX_BCAST_INVALID;

    return ir1_opnd;
}
static void __attribute__((__constructor__)) x86tomisp_ir1_init(void)
{
    al_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_AL);
    ah_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_AH);
    ax_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_AX);
    eax_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_EAX);
    rax_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RAX);

    cl_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_CL);
    ch_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_CH);
    cx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_CX);
    ecx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_ECX);
    rcx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RCX);

    dl_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_DL);
    dh_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_DH);
    dx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_DX);
    edx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_EDX);
    rdx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RDX);

    bl_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_BL);
    bh_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 8, X86_REG_BH);
    bx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_BX);
    ebx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_EBX);
    rbx_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RBX);

    sp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_ESP);
    esp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_ESP);
    rsp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RSP);

    bp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_EBP);
    ebp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_EBP);
    rbp_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RBP);

    si_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_SI);
    esi_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_ESI);
    rsi_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RSI);

    di_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 16, X86_REG_DI);
    edi_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 32, X86_REG_EDI);
    rdi_ir1_opnd = ir1_opnd_new_static_reg(X86_OP_REG, 64, X86_REG_RDI);

    eax_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_EAX, 0);
    ecx_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_ECX, 0);
    edx_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_EDX, 0);
    ebx_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_EBX, 0);
    esp_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_ESP, 0);
    ebp_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_EBP, 0);
    esi_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_ESI, 0);
    edi_mem8_ir1_opnd = ir1_opnd_new_static_mem(X86_OP_MEM, 8, X86_REG_EDI, 0);

    eax_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_EAX, 0);
    ecx_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_ECX, 0);
    edx_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_EDX, 0);
    ebx_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_EBX, 0);
    esp_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_ESP, 0);
    ebp_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_EBP, 0);
    esi_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_ESI, 0);
    edi_mem16_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 16, X86_REG_EDI, 0);

    eax_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_EAX, 0);
    ecx_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_ECX, 0);
    edx_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_EDX, 0);
    ebx_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_EBX, 0);
    esp_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_ESP, 0);
    ebp_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_EBP, 0);
    esi_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_ESI, 0);
    edi_mem32_ir1_opnd =
        ir1_opnd_new_static_mem(X86_OP_MEM, 32, X86_REG_EDI, 0);
};

int ir1_opnd_index_reg_num(IR1_OPND *opnd)
{
    switch (opnd->mem.index) {
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_RAX:
        return eax_index;
    case X86_REG_BL:
    case X86_REG_BH:
    case X86_REG_BX:
    case X86_REG_EBX:
    case X86_REG_RBX:
        return ebx_index;
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_RCX:
        return ecx_index;
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:
    case X86_REG_RDX:
        return edx_index;
    case X86_REG_BP:
    case X86_REG_EBP:
    case X86_REG_RBP:
    case X86_REG_BPL:
        return ebp_index;
    case X86_REG_SI:
    case X86_REG_ESI:
    case X86_REG_RSI:
    case X86_REG_SIL:
        return esi_index;
    case X86_REG_DI:
    case X86_REG_EDI:
    case X86_REG_RDI:
    case X86_REG_DIL:
        return edi_index;
    case X86_REG_SP:
    case X86_REG_ESP:
    case X86_REG_RSP:
    case X86_REG_SPL:
        return esp_index;
    case X86_REG_R8B:
    case X86_REG_R8W:
    case X86_REG_R8D:
    case X86_REG_R8:
        return r8_index;
    case X86_REG_R9B:
    case X86_REG_R9W:
    case X86_REG_R9D:
    case X86_REG_R9:
        return r9_index;
    case X86_REG_R10B:
    case X86_REG_R10W:
    case X86_REG_R10D:
    case X86_REG_R10:
        return r10_index;
    case X86_REG_R11B:
    case X86_REG_R11W:
    case X86_REG_R11D:
    case X86_REG_R11:
        return r11_index;
    case X86_REG_R12B:
    case X86_REG_R12W:
    case X86_REG_R12D:
    case X86_REG_R12:
        return r12_index;
    case X86_REG_R13B:
    case X86_REG_R13W:
    case X86_REG_R13D:
    case X86_REG_R13:
        return r13_index;
    case X86_REG_R14B:
    case X86_REG_R14W:
    case X86_REG_R14D:
    case X86_REG_R14:
        return r14_index;
    case X86_REG_R15B:
    case X86_REG_R15W:
    case X86_REG_R15D:
    case X86_REG_R15:
        return r15_index;
    case X86_REG_CS:
        return cs_index;
    case X86_REG_DS:
        return ds_index;
    case X86_REG_SS:
        return ss_index;
    case X86_REG_ES:
        return es_index;
    case X86_REG_GS:
        return gs_index;
    case X86_REG_FS:
        return fs_index;

    default:
        lsassert(0);
        // may fpu?
        return -1;
    }
}
int ir1_opnd_base_reg_num(IR1_OPND *opnd)
{
    // xtm : base may not a mem opnd
    if (opnd->type == X86_OP_MEM && opnd->mem.base == X86_REG_INVALID) {
        return -1;
    }
    //    lsassert(opnd->type == X86_OP_MEM || opnd->type == X86_OP_REG);
    switch (opnd->type == X86_OP_REG ? opnd->reg : opnd->mem.base) {
    case X86_REG_RIP:
    case X86_REG_EIP:
        printf("%s:%d mem base RIP(EIP).\n", __func__, __LINE__);
        return -1;
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_RAX:
        return eax_index;
    case X86_REG_BL:
    case X86_REG_BH:
    case X86_REG_BX:
    case X86_REG_EBX:
    case X86_REG_RBX:
        return ebx_index;
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_RCX:
        return ecx_index;
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:
    case X86_REG_RDX:
        return edx_index;
    case X86_REG_BP:
    case X86_REG_EBP:
    case X86_REG_RBP:
    case X86_REG_BPL:
        return ebp_index;
    case X86_REG_SI:
    case X86_REG_ESI:
    case X86_REG_RSI:
    case X86_REG_SIL:
        return esi_index;
    case X86_REG_DI:
    case X86_REG_EDI:
    case X86_REG_RDI:
    case X86_REG_DIL:
        return edi_index;
    case X86_REG_SP:
    case X86_REG_ESP:
    case X86_REG_RSP:
    case X86_REG_SPL:
        return esp_index;
    case X86_REG_R8B:
    case X86_REG_R8W:
    case X86_REG_R8D:
    case X86_REG_R8:
        return r8_index;
    case X86_REG_R9B:
    case X86_REG_R9W:
    case X86_REG_R9D:
    case X86_REG_R9:
        return r9_index;
    case X86_REG_R10B:
    case X86_REG_R10W:
    case X86_REG_R10D:
    case X86_REG_R10:
        return r10_index;
    case X86_REG_R11B:
    case X86_REG_R11W:
    case X86_REG_R11D:
    case X86_REG_R11:
        return r11_index;
    case X86_REG_R12B:
    case X86_REG_R12W:
    case X86_REG_R12D:
    case X86_REG_R12:
        return r12_index;
    case X86_REG_R13B:
    case X86_REG_R13W:
    case X86_REG_R13D:
    case X86_REG_R13:
        return r13_index;
    case X86_REG_R14B:
    case X86_REG_R14W:
    case X86_REG_R14D:
    case X86_REG_R14:
        return r14_index;
    case X86_REG_R15B:
    case X86_REG_R15W:
    case X86_REG_R15D:
    case X86_REG_R15:
        return r15_index;
    case X86_REG_CS:
        return cs_index;
    case X86_REG_DS:
        return ds_index;
    case X86_REG_SS:
        return ss_index;
    case X86_REG_ES:
        return es_index;
    case X86_REG_GS:
        return gs_index;
    case X86_REG_FS:
        return fs_index;
    case X86_REG_MM0:
        return 0;
    case X86_REG_MM1:
        return 1;
    case X86_REG_MM2:
        return 2;
    case X86_REG_MM3:
        return 3;
    case X86_REG_MM4:
        return 4;
    case X86_REG_MM5:
        return 5;
    case X86_REG_MM6:
        return 6;
    case X86_REG_MM7:
        return 7;
    case X86_REG_XMM0:
        return 0;
    case X86_REG_XMM1:
        return 1;
    case X86_REG_XMM2:
        return 2;
    case X86_REG_XMM3:
        return 3;
    case X86_REG_XMM4:
        return 4;
    case X86_REG_XMM5:
        return 5;
    case X86_REG_XMM6:
        return 6;
    case X86_REG_XMM7:
        return 7;
    case X86_REG_YMM0:
        return 0;
    case X86_REG_YMM1:
        return 1;
    case X86_REG_YMM2:
        return 2;
    case X86_REG_YMM3:
        return 3;
    case X86_REG_YMM4:
        return 4;
    case X86_REG_YMM5:
        return 5;
    case X86_REG_YMM6:
        return 6;
    case X86_REG_YMM7:
        return 7;
    case X86_REG_XMM8:
        return 8;
    case X86_REG_XMM9:
        return 9;
    case X86_REG_XMM10:
        return 10;
    case X86_REG_XMM11:
        return 11;
    case X86_REG_XMM12:
        return 12;
    case X86_REG_XMM13:
        return 13;
    case X86_REG_XMM14:
        return 14;
    case X86_REG_XMM15:
        return 15;
    case X86_REG_YMM8:
        return 8;
    case X86_REG_YMM9:
        return 9;
    case X86_REG_YMM10:
        return 10;
    case X86_REG_YMM11:
        return 11;
    case X86_REG_YMM12:
        return 12;
    case X86_REG_YMM13:
        return 13;
    case X86_REG_YMM14:
        return 14;
    case X86_REG_YMM15:
        return 15;
    case X86_REG_ST0:
        return 0;
    case X86_REG_ST1:
        return 1;
    case X86_REG_ST2:
        return 2;
    case X86_REG_ST3:
        return 3;
    case X86_REG_ST4:
        return 4;
    case X86_REG_ST5:
        return 5;
    case X86_REG_ST6:
        return 6;
    case X86_REG_ST7:
        return 7;
    default:
        lsassert(0);
        return -1;
    }
}

longx ir1_opnd_simm(IR1_OPND *opnd)
{
    if (ir1_opnd_type(opnd) == X86_OP_IMM) {
        switch (ir1_opnd_size(opnd)) {
        case 8:
            return (longx)((int8_t)(opnd->imm));
        case 16:
            return (longx)((int16_t)(opnd->imm));
        case 32:
            return (longx)((int32_t)(opnd->imm));
        case 64:
            return (longx)((int64_t)(opnd->imm));
        default:
            lsassert(0);
        }
    } else if (ir1_opnd_type(opnd) == X86_OP_MEM) {
        return (longx)(opnd->mem.disp);
    } else {
        lsassertm(0, "REG opnd has no imm\n");
    }
    abort();
}

longx ir1_opnd_u2simm(IR1_OPND *opnd)
{
    lsassert(ir1_opnd_is_imm(opnd));
    switch (ir1_opnd_size(opnd)) {
    case 8:
        return (longx)((uint8_t)(opnd->imm));
    case 16:
        return (longx)((uint16_t)(opnd->imm));
    case 32:
        return (longx)((uint32_t)(opnd->imm));
    case 64:
        return (longx)((uint64_t)(opnd->imm));
    default:
        lsassert(0);
    }
    abort();
}

ulongx ir1_opnd_s2uimm(IR1_OPND *opnd)
{
    lsassert(ir1_opnd_is_imm(opnd));
    switch (ir1_opnd_size(opnd)) {
    case 8:
        return (ulongx)((int8_t)(opnd->imm));
    case 16:
        return (ulongx)((int16_t)(opnd->imm));
    case 32:
        return (ulongx)((int32_t)(opnd->imm));
    case 64:
        return (ulongx)((int64_t)(opnd->imm));
    default:
        lsassert(0);
    }
    abort();
}

int ir1_opnd_is_8l(IR1_OPND *opnd)
{
    return opnd->type == X86_OP_REG && opnd->size == 1 &&
           (opnd->reg == X86_REG_AL   || opnd->reg == X86_REG_BL   ||
            opnd->reg == X86_REG_CL   || opnd->reg == X86_REG_DL   ||
            opnd->reg == X86_REG_R8B  || opnd->reg == X86_REG_R9B  ||
            opnd->reg == X86_REG_R10B || opnd->reg == X86_REG_R11B ||
            opnd->reg == X86_REG_R12B || opnd->reg == X86_REG_R13B ||
            opnd->reg == X86_REG_R14B || opnd->reg == X86_REG_R15B ||
            opnd->reg == X86_REG_SPL  || opnd->reg == X86_REG_BPL  ||
            opnd->reg == X86_REG_SIL  || opnd->reg == X86_REG_DIL);
}

int ir1_opnd_is_16l(IR1_OPND *opnd)
{
    return opnd->type == X86_OP_REG && opnd->size == 2 &&
           (opnd->reg == X86_REG_AX   || opnd->reg == X86_REG_BX   ||
            opnd->reg == X86_REG_CX   || opnd->reg == X86_REG_DX   ||
            opnd->reg == X86_REG_R8W  || opnd->reg == X86_REG_R9W  ||
            opnd->reg == X86_REG_R10W || opnd->reg == X86_REG_R11W ||
            opnd->reg == X86_REG_R12W || opnd->reg == X86_REG_R13W ||
            opnd->reg == X86_REG_R14W || opnd->reg == X86_REG_R15W ||
            opnd->reg == X86_REG_SP   || opnd->reg == X86_REG_BP   ||
            opnd->reg == X86_REG_SI   || opnd->reg == X86_REG_DI);
}

int ir1_opnd_is_gpr(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_BL:
    case X86_REG_BH:
    case X86_REG_BX:
    case X86_REG_EBX:
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:

    case X86_REG_BP:
    case X86_REG_EBP:
    case X86_REG_SI:
    case X86_REG_ESI:
    case X86_REG_DI:
    case X86_REG_EDI:
    case X86_REG_SP:
    case X86_REG_ESP:
    case X86_REG_RAX:
    case X86_REG_RBX:
    case X86_REG_RCX:
    case X86_REG_RDX:

    case X86_REG_R8 ... X86_REG_R15:
    case X86_REG_R8D ... X86_REG_R15D:
    case X86_REG_R8W ... X86_REG_R15W:
    case X86_REG_R8B ... X86_REG_R15B:

    case X86_REG_RBP:
    case X86_REG_RSI:
    case X86_REG_RDI:
    case X86_REG_RSP:
    case X86_REG_BPL:
    case X86_REG_SIL:
    case X86_REG_DIL:
    case X86_REG_SPL:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_uimm_within_16bit(IR1_OPND *opnd)
{
    return ir1_opnd_is_imm(opnd) && ir1_opnd_uimm(opnd) < 4096;
}

int ir1_opnd_is_simm_within_12bit(IR1_OPND *opnd)
{
    return ir1_opnd_is_imm(opnd) && ir1_opnd_simm(opnd) >= -2048&&
           ir1_opnd_simm(opnd) < 2047;
}

bool ir1_opnd_is_uimm12(IR1_OPND *opnd)
{
    return ir1_opnd_is_imm(opnd) && ir1_opnd_uimm(opnd) < 4096;
}

bool ir1_opnd_is_simm12(IR1_OPND *opnd)
{
    return ir1_opnd_is_imm(opnd) && ir1_opnd_simm(opnd) >= -2048 &&
           ir1_opnd_simm(opnd) < 2047;
}

bool ir1_opnd_is_s2uimm12(IR1_OPND *opnd)
{
    return ir1_opnd_is_imm(opnd) && ir1_opnd_s2uimm(opnd) < 4096;
}

bool ir1_opnd_is_u2simm12(IR1_OPND *opnd)
{
    lsassert(ir1_opnd_is_imm(opnd) && ir1_opnd_u2simm(opnd) >= 0);
    return ir1_opnd_is_imm(opnd) && ir1_opnd_u2simm(opnd) < 2047;
}

int ir1_opnd_is_fpr(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_ST0:
    case X86_REG_ST1:
    case X86_REG_ST2:
    case X86_REG_ST3:
    case X86_REG_ST4:
    case X86_REG_ST5:
    case X86_REG_ST6:
    case X86_REG_ST7:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_seg(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_CS:
    case X86_REG_DS:
    case X86_REG_SS:
    case X86_REG_ES:
    case X86_REG_FS:
    case X86_REG_GS:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_mmx(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_MM0:
    case X86_REG_MM1:
    case X86_REG_MM2:
    case X86_REG_MM3:
    case X86_REG_MM4:
    case X86_REG_MM5:
    case X86_REG_MM6:
    case X86_REG_MM7:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_xmm(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_XMM0:
    case X86_REG_XMM1:
    case X86_REG_XMM2:
    case X86_REG_XMM3:
    case X86_REG_XMM4:
    case X86_REG_XMM5:
    case X86_REG_XMM6:
    case X86_REG_XMM7:
    case X86_REG_XMM8:
    case X86_REG_XMM9:
    case X86_REG_XMM10:
    case X86_REG_XMM11:
    case X86_REG_XMM12:
    case X86_REG_XMM13:
    case X86_REG_XMM14:
    case X86_REG_XMM15:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_ymm(IR1_OPND *opnd)
{
    if (opnd->type != X86_OP_REG) {
        return 0;
    }
    switch (opnd->reg) {
    case X86_REG_YMM0:
    case X86_REG_YMM1:
    case X86_REG_YMM2:
    case X86_REG_YMM3:
    case X86_REG_YMM4:
    case X86_REG_YMM5:
    case X86_REG_YMM6:
    case X86_REG_YMM7:
    case X86_REG_YMM8:
    case X86_REG_YMM9:
    case X86_REG_YMM10:
    case X86_REG_YMM11:
    case X86_REG_YMM12:
    case X86_REG_YMM13:
    case X86_REG_YMM14:
    case X86_REG_YMM15:
        return 1;
    default:
        return 0;
    }
}

int ir1_opnd_is_pc_relative(IR1_OPND *opnd)
{
    /* x86_64 : RIP/EIP relative addressing */
    x86_reg base = ir1_opnd_base_reg(opnd);
    if (base == X86_REG_RIP) {
        return 1;
    } else if (base == X86_REG_EIP) {
        /* legal but strange */
        return 2;
    } else {
        return 0;
    }
}

int ir1_opnd_is_same_reg(IR1_OPND *opnd0, IR1_OPND *opnd1)
{
    return ir1_opnd_is_gpr(opnd0) && ir1_opnd_is_gpr(opnd1) &&
        opnd0->reg == opnd1->reg;
}

int ir1_opnd_is_same_reg_without_width(IR1_OPND *opnd0, IR1_OPND *opnd1)
{
    lsassert(opnd0->type == X86_OP_REG && opnd1->type == X86_OP_REG);

    int opnd0_index = 0;
    int opnd1_index = 0;

    switch (opnd0->reg) {
    case X86_REG_RIP:
    case X86_REG_EIP:
        opnd0_index = -1;   break;
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_RAX:
        opnd0_index = eax_index;   break;
    case X86_REG_BL:
    case X86_REG_BH:
    case X86_REG_BX:
    case X86_REG_EBX:
    case X86_REG_RBX:
        opnd0_index = ebx_index;   break;
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_RCX:
        opnd0_index = ecx_index;   break;
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:
    case X86_REG_RDX:
        opnd0_index = edx_index;   break;
    case X86_REG_BP:
    case X86_REG_EBP:
    case X86_REG_RBP:
    case X86_REG_BPL:
        opnd0_index = ebp_index;   break;
    case X86_REG_SI:
    case X86_REG_ESI:
    case X86_REG_RSI:
    case X86_REG_SIL:
        opnd0_index = esi_index;   break;
    case X86_REG_DI:
    case X86_REG_EDI:
    case X86_REG_RDI:
    case X86_REG_DIL:
        opnd0_index = edi_index;   break;
    case X86_REG_SP:
    case X86_REG_ESP:
    case X86_REG_RSP:
    case X86_REG_SPL:
        opnd0_index = esp_index;   break;
    case X86_REG_R8B:
    case X86_REG_R8W:
    case X86_REG_R8D:
    case X86_REG_R8:
        opnd0_index = r8_index;   break;
    case X86_REG_R9B:
    case X86_REG_R9W:
    case X86_REG_R9D:
    case X86_REG_R9:
        opnd0_index = r9_index;   break;
    case X86_REG_R10B:
    case X86_REG_R10W:
    case X86_REG_R10D:
    case X86_REG_R10:
        opnd0_index = r10_index;   break;
    case X86_REG_R11B:
    case X86_REG_R11W:
    case X86_REG_R11D:
    case X86_REG_R11:
        opnd0_index = r11_index;   break;
    case X86_REG_R12B:
    case X86_REG_R12W:
    case X86_REG_R12D:
    case X86_REG_R12:
        opnd0_index = r12_index;   break;
    case X86_REG_R13B:
    case X86_REG_R13W:
    case X86_REG_R13D:
    case X86_REG_R13:
        opnd0_index = r13_index;   break;
    case X86_REG_R14B:
    case X86_REG_R14W:
    case X86_REG_R14D:
    case X86_REG_R14:
        opnd0_index = r14_index;   break;
    case X86_REG_R15B:
    case X86_REG_R15W:
    case X86_REG_R15D:
    case X86_REG_R15:
        opnd0_index = r15_index;   break;
    default:
        printf("%s:%d reg no match.\n", __func__, __LINE__);
        lsassert(0);
    }
    switch (opnd1->reg) {
    case X86_REG_RIP:
    case X86_REG_EIP:
        opnd1_index = -1;   break;
    case X86_REG_AL:
    case X86_REG_AH:
    case X86_REG_AX:
    case X86_REG_EAX:
    case X86_REG_RAX:
        opnd1_index = eax_index;   break;
    case X86_REG_BL:
    case X86_REG_BH:
    case X86_REG_BX:
    case X86_REG_EBX:
    case X86_REG_RBX:
        opnd1_index = ebx_index;   break;
    case X86_REG_CL:
    case X86_REG_CH:
    case X86_REG_CX:
    case X86_REG_ECX:
    case X86_REG_RCX:
        opnd1_index = ecx_index;   break;
    case X86_REG_DL:
    case X86_REG_DH:
    case X86_REG_DX:
    case X86_REG_EDX:
    case X86_REG_RDX:
        opnd1_index = edx_index;   break;
    case X86_REG_BP:
    case X86_REG_EBP:
    case X86_REG_RBP:
    case X86_REG_BPL:
        opnd1_index = ebp_index;   break;
    case X86_REG_SI:
    case X86_REG_ESI:
    case X86_REG_RSI:
    case X86_REG_SIL:
        opnd1_index = esi_index;   break;
    case X86_REG_DI:
    case X86_REG_EDI:
    case X86_REG_RDI:
    case X86_REG_DIL:
        opnd1_index = edi_index;   break;
    case X86_REG_SP:
    case X86_REG_ESP:
    case X86_REG_RSP:
    case X86_REG_SPL:
        opnd1_index = esp_index;   break;
    case X86_REG_R8B:
    case X86_REG_R8W:
    case X86_REG_R8D:
    case X86_REG_R8:
        opnd1_index = r8_index;   break;
    case X86_REG_R9B:
    case X86_REG_R9W:
    case X86_REG_R9D:
    case X86_REG_R9:
        opnd1_index = r9_index;   break;
    case X86_REG_R10B:
    case X86_REG_R10W:
    case X86_REG_R10D:
    case X86_REG_R10:
        opnd1_index = r10_index;   break;
    case X86_REG_R11B:
    case X86_REG_R11W:
    case X86_REG_R11D:
    case X86_REG_R11:
        opnd1_index = r11_index;   break;
    case X86_REG_R12B:
    case X86_REG_R12W:
    case X86_REG_R12D:
    case X86_REG_R12:
        opnd1_index = r12_index;   break;
    case X86_REG_R13B:
    case X86_REG_R13W:
    case X86_REG_R13D:
    case X86_REG_R13:
        opnd1_index = r13_index;   break;
    case X86_REG_R14B:
    case X86_REG_R14W:
    case X86_REG_R14D:
    case X86_REG_R14:
        opnd1_index = r14_index;   break;
    case X86_REG_R15B:
    case X86_REG_R15W:
    case X86_REG_R15D:
    case X86_REG_R15:
        opnd1_index = r15_index;   break;
    default:
        printf("%s:%d reg no match.\n", __func__, __LINE__);
        lsassert(0);
    }

    return opnd0_index == opnd1_index;
}

int ir1_opnd_get_seg_index(IR1_OPND *opnd)
{
    lsassert(ir1_opnd_type(opnd) == X86_OP_MEM);
    x86_reg seg = opnd->mem.segment;
    if (seg == X86_REG_ES) {
        return es_index;
    }
    if (seg == X86_REG_CS) {
        return cs_index;
    }
    if (seg == X86_REG_SS) {
        return ss_index;
    }
    if (seg == X86_REG_DS) {
        return ds_index;
    }
    if (seg == X86_REG_FS) {
        return fs_index;
    }
    if (seg == X86_REG_GS) {
        return gs_index;
    }
    abort();
    //     IR1_OPND_MEM_ES,
    //     IR1_OPND_MEM_CS,
    //     IR1_OPND_MEM_SS,
    //     IR1_OPND_MEM_DS,
    //     IR1_OPND_MEM_FS,
    //     IR1_OPND_MEM_GS,
}

ADDRX ir1_target_addr(IR1_INST *ir1)
{
    lsassert(ir1_opnd_type(&(ir1->info->detail->x86.operands[0])) ==
             X86_OP_IMM);
    return (ADDRX)ir1_opnd_uimm(&(ir1->info->detail->x86.operands[0]));
}

// IR1_OPCODE ir1_opcode(IR1_INST *ir1){
//     return ir1->detail->x86.opcode;
// }

bool ir1_is_tb_ending(IR1_INST *ir1)
{
    // return ir1_is_branch(ir1) || ir1_is_jump(ir1) || ir1_is_call(ir1) ||
    // ir1_is_return(ir1) || ir1_is_indirect(ir1) || ir1_is_syscall(ir1);
    if (ir1_opcode(ir1) == X86_INS_CALL && !ir1_is_indirect_call(ir1) &&
        ir1_addr_next(ir1) == ir1_target_addr(ir1)) {
        return true;
    }
    /*
     * INTO only raises #OF when OF is set.  When OF is clear the helper
     * returns and execution continues at the next instruction, so it cannot
     * end a TB unless a real control-flow instruction provides an exit.
     */
    if (latxs_ir1_is_int(ir1) && ir1_opcode(ir1) != X86_INS_INTO) {
        return true;
    }
    return ir1_is_branch(ir1) || ir1_is_jump(ir1) || ir1_is_call(ir1) ||
           ir1_is_return(ir1) || ir1_is_syscall(ir1);
}

#if defined(CONFIG_LATX_PRINT) || !defined(CONFIG_CAPSTONE_DIET)
int ir1_dump(IR1_INST *ir1)
{
    fprintf(stderr, "0x%" PRIx64 ": ", ir1->info->address);
    int i = 0;
    for (; i < ir1->info->size; i++) {
        fprintf(stderr, "%02x", ir1->info->bytes[i]);
    }
    for (; i < 16; i++) {
        fprintf(stderr, "  ");
    }
    fprintf(stderr, "\tAS=%1d ", latxs_ir1_addr_size(ir1));

    fprintf(stderr, "%s\t\t%s\t", ir1->info->mnemonic, ir1->info->op_str);

    fprintf(stderr, "[%x %x]\t", ir1->_eflag_use, ir1->_eflag_def);

#ifndef CONFIG_CAPSTONE_DIET
    int grp_nr = ir1_grp_nr(ir1);
    uint8_t *grps = ir1_get_grps(ir1);
    if (grp_nr > 0) {
        fprintf(stderr, "\tGROUP [");
        for (i = 0; i < grp_nr - 1; ++i) {
            fprintf(stderr, "%d,", grps[i]);
        }
        fprintf(stderr, "%d]\n", grps[i]);
    } else {
        fprintf(stderr, "\n");
    }
#else
        fprintf(stderr, "\n");
#endif
    return 0;
}

int ir1_opcode_dump(IR1_INST *ir1)
{
    fprintf(stderr, "0x%" PRIx64 ":\t%s\n", ir1->info->address,
            ir1->info->mnemonic);
    return 0;
}
#endif

const char * ir1_name(IR1_OPCODE op){
    return cs_insn_name(handle, op);
}

const char * ir1_group_name(x86_insn_group grp){
    return cs_group_name(handle, grp);
}

const char *ir1_reg_name(x86_reg reg) { return cs_reg_name(handle, reg); }

int latxs_ir1_mem_opnd_is_base_imm(IR1_OPND *opnd)
{
    if (!ir1_opnd_has_index(opnd) && ir1_opnd_has_base(opnd) &&
        !ir1_opnd_has_seg(opnd)) {
        return 1;
    }
    return 0;
}

int latxs_ir1_opnd_is_cr(IR1_OPND *opnd)
{
    if (ir1_opnd_type(opnd) != X86_OP_REG) {
        return 0;
    }

    switch (opnd->reg) {
    case X86_REG_CR0:
    case X86_REG_CR1:
    case X86_REG_CR2:
    case X86_REG_CR3:
    case X86_REG_CR4:
    case X86_REG_CR5:
    case X86_REG_CR6:
    case X86_REG_CR7:
    case X86_REG_CR8:
    case X86_REG_CR9:
    case X86_REG_CR10:
    case X86_REG_CR11:
    case X86_REG_CR12:
    case X86_REG_CR13:
    case X86_REG_CR14:
    case X86_REG_CR15:
        return 1;
    default:
        return 0;
    }
}

int latxs_ir1_opnd_get_cr_num(IR1_OPND *opnd, int *flag)
{
    switch (opnd->reg) {
    case X86_REG_CR0:
    case X86_REG_CR2:
    case X86_REG_CR3:
    case X86_REG_CR4:
    case X86_REG_CR8:
        *flag = 1; /* OK to read/write CR */
        return opnd->reg - X86_REG_CR0;
    case X86_REG_CR1:
    case X86_REG_CR5:
    case X86_REG_CR6:
    case X86_REG_CR7:
    case X86_REG_CR9:
    case X86_REG_CR10:
    case X86_REG_CR11:
    case X86_REG_CR12:
    case X86_REG_CR13:
    case X86_REG_CR14:
    case X86_REG_CR15:
        return opnd->reg - X86_REG_CR0;
    default:
        return -1;
    }
}

int latxs_ir1_is_mov_to_cr8(IR1_INST *ir1)
{
    int flag;
    return ir1_opcode(ir1) == X86_INS_MOV &&
        latxs_ir1_opnd_is_cr(ir1_get_opnd(ir1, 0)) &&
        (latxs_ir1_opnd_get_cr_num(ir1_get_opnd(ir1, 0), &flag) == 8);
}

int latxs_ir1_opnd_is_dr(IR1_OPND *opnd)
{
    if (ir1_opnd_type(opnd) != X86_OP_REG) {
        return 0;
    }

    switch (opnd->reg) {
    case X86_REG_DR0:
    case X86_REG_DR1:
    case X86_REG_DR2:
    case X86_REG_DR3:
    case X86_REG_DR4:
    case X86_REG_DR5:
    case X86_REG_DR6:
    case X86_REG_DR7:
    case X86_REG_DR8:
    case X86_REG_DR9:
    case X86_REG_DR10:
    case X86_REG_DR11:
    case X86_REG_DR12:
    case X86_REG_DR13:
    case X86_REG_DR14:
    case X86_REG_DR15:
        return 1;
    default:
        return 0;
    }
}

int latxs_ir1_opnd_get_dr_num(IR1_OPND *opnd)
{
    switch (opnd->reg) {
    case X86_REG_DR0:
    case X86_REG_DR1:
    case X86_REG_DR2:
    case X86_REG_DR3:
    case X86_REG_DR4:
    case X86_REG_DR5:
    case X86_REG_DR6:
    case X86_REG_DR7:
    case X86_REG_DR8:
    case X86_REG_DR9:
    case X86_REG_DR10:
    case X86_REG_DR11:
    case X86_REG_DR12:
    case X86_REG_DR13:
    case X86_REG_DR14:
    case X86_REG_DR15:
        return opnd->reg - X86_REG_DR0;
    default:
        return -1;
    }
}

int latxs_ir1_is_mov_to_cr(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_MOV &&
        latxs_ir1_opnd_is_cr(ir1_get_opnd(ir1, 0));
}
int latxs_ir1_is_mov_from_cr(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_MOV &&
        latxs_ir1_opnd_is_cr(ir1_get_opnd(ir1, 1));
}
int latxs_ir1_is_mov_to_dr(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_MOV &&
        latxs_ir1_opnd_is_dr(ir1_get_opnd(ir1, 0));
}
int latxs_ir1_is_mov_to_seg(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_MOV &&
        ir1_opnd_is_seg(ir1_get_opnd(ir1, 0));
}
int latxs_ir1_is_pop_to_seg(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_POP &&
        ir1_opnd_is_seg(ir1_get_opnd(ir1, 0));
}

int latxs_ir1_is_repz_nop(IR1_INST *ir1)
{
    if (!latxs_ir1_is_nop(ir1)) {
        return 0;
    }

    /*
     * might be a bug of capstone:
     * For inst '0xf3 0x90' the prefix in cs_insn
     * will not be setted, but the bytes is OK.
     */
    uint8_t  *bytes = (uint8_t *)(ir1->info->bytes);
    if (bytes[0] == X86_PREFIX_REPE) {
        return 1;
    }

    return 0;
}

int latxs_ir1_access_mmx(IR1_INST *ir1)
{
    int opnd_num = ir1_opnd_num(ir1);
    for (int i = 0; i < opnd_num; i++) {
        if (ir1_opnd_is_mmx(ir1_get_opnd(ir1, i))) {
            return 1;
        }
    }
    return 0;
}
