/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "lsenv.h"
#include "reg-alloc.h"
#include "flag-lbt.h"
#include <string.h>
#include "latx-config.h"
#include "latx-options.h"
#include "sys-aot.h"
#include "fpu/softfloat.h"
#include "translate.h"
#include "latx-misc.h"
#include "latx-static-codes.h"
#include "latx-sys-inst-ptn.h"
#include "latx-sys-tr-sse41.h"
#include "latx-sys-tr-sse42.h"
#include "latx-sys-tr-ssse3.h"

bool ir1_need_calculate_cf(IR1_INST *ir1)
{
    return (ir1_is_cf_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << CF_USEDEF_BIT_INDEX)) ||
                          ir1_opcode(ir1) == X86_INS_LZCNT;
}

bool ir1_need_calculate_pf(IR1_INST *ir1)
{
    return ir1_is_pf_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << PF_USEDEF_BIT_INDEX);
}

bool ir1_need_calculate_af(IR1_INST *ir1)
{
    return ir1_is_af_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << AF_USEDEF_BIT_INDEX);
}

bool ir1_need_calculate_zf(IR1_INST *ir1)
{
    return ir1_is_zf_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << ZF_USEDEF_BIT_INDEX);
}

bool ir1_need_calculate_sf(IR1_INST *ir1)
{
    return ir1_is_sf_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << SF_USEDEF_BIT_INDEX);
}

bool ir1_need_calculate_of(IR1_INST *ir1)
{
    return ir1_is_of_def(ir1) &&
           BITS_ARE_CLEAR(lsenv->tr_data->curr_ir1_skipped_eflags,
                          1 << OF_USEDEF_BIT_INDEX);
}

bool ir1_need_calculate_any_flag(IR1_INST *ir1)
{
    return (ir1_get_eflag_def(ir1) &
            ~(lsenv->tr_data->curr_ir1_skipped_eflags)) != 0;
}

static bool latxs_translate_to_null(IR1_INST *pir1)
{
    return true;
}

#define DEF_TR_FUNC(opcode, func) [X86_INS_##opcode] = latxs_translate_##func

int idx_start = __LINE__ + 5;
static bool (*translate_functions[])(IR1_INST *) = {
    DEF_TR_FUNC(INVALID,    invalid),

    DEF_TR_FUNC(AAA,    aaa),
    DEF_TR_FUNC(AAD,    aad),
    DEF_TR_FUNC(AAM,    aam),
    DEF_TR_FUNC(AAS,    aas),
    DEF_TR_FUNC(DAA,    daa),
    DEF_TR_FUNC(DAS,    das),

    DEF_TR_FUNC(ADC,    adc),
    DEF_TR_FUNC(SBB,    sbb),

    DEF_TR_FUNC(INC,    inc),
    DEF_TR_FUNC(DEC,    dec),

    DEF_TR_FUNC(ADD,    add),
    DEF_TR_FUNC(ADDPD,  addpd),
    DEF_TR_FUNC(ADDPS,  addps),
    DEF_TR_FUNC(ADDSD,  addsd),
    DEF_TR_FUNC(ADDSS,  addss),

    DEF_TR_FUNC(AND,    and),
    DEF_TR_FUNC(ANDNPD, andnpd),
    DEF_TR_FUNC(ANDNPS, andnps),
    DEF_TR_FUNC(ANDPD,  andpd),
    DEF_TR_FUNC(ANDPS,  andps),

    DEF_TR_FUNC(BSF,    bsf),
    DEF_TR_FUNC(BSR,    bsr),
    DEF_TR_FUNC(BSWAP,  bswap),
    DEF_TR_FUNC(BT,     btx),
    DEF_TR_FUNC(BTC,    btx),
    DEF_TR_FUNC(BTR,    btx),
    DEF_TR_FUNC(BTS,    btx),

    DEF_TR_FUNC(JMP,    jmp),
    DEF_TR_FUNC(CALL,   call),
    DEF_TR_FUNC(RET,    ret),
    DEF_TR_FUNC(LCALL,  lcall),
    DEF_TR_FUNC(LJMP,   jmp_far),
    DEF_TR_FUNC(RETF,   retf),
    DEF_TR_FUNC(RETFQ,  retf),

    DEF_TR_FUNC(CLAC,   clac),
    DEF_TR_FUNC(STAC,   stac),

    DEF_TR_FUNC(CLC,    clc),
    DEF_TR_FUNC(STC,    stc),
    DEF_TR_FUNC(CLD,    cld),
    DEF_TR_FUNC(STD,    std),

    DEF_TR_FUNC(STI,    sti),
    DEF_TR_FUNC(CLI,    cli),

    DEF_TR_FUNC(CMC,    cmc),
    DEF_TR_FUNC(CMOVA,  cmova),
    DEF_TR_FUNC(CMOVAE, cmovae),
    DEF_TR_FUNC(CMOVB,  cmovb),
    DEF_TR_FUNC(CMOVBE, cmovbe),
    DEF_TR_FUNC(CMOVE,  cmovz),
    DEF_TR_FUNC(FCMOVE, fcmovcc),
    DEF_TR_FUNC(CMOVG,  cmovg),
    DEF_TR_FUNC(CMOVGE, cmovge),
    DEF_TR_FUNC(CMOVL,  cmovl),
    DEF_TR_FUNC(CMOVLE, cmovle),
    DEF_TR_FUNC(CMOVNE, cmovnz),
    DEF_TR_FUNC(CMOVNO, cmovno),
    DEF_TR_FUNC(CMOVNP, cmovnp),
    DEF_TR_FUNC(CMOVNS, cmovns),
    DEF_TR_FUNC(CMOVO,  cmovo),
    DEF_TR_FUNC(CMOVP,  cmovp),
    DEF_TR_FUNC(FCMOVU, fcmovcc),
    DEF_TR_FUNC(CMOVS,  cmovs),

    DEF_TR_FUNC(FABS,   fabs),
    DEF_TR_FUNC(FADD,   fadd),
    DEF_TR_FUNC(FIADD,  fiadd),
    DEF_TR_FUNC(FADDP,  faddp),
    DEF_TR_FUNC(FCHS,   fchs),
    DEF_TR_FUNC(FCMOVBE,    fcmovcc),
    DEF_TR_FUNC(FCMOVB,     fcmovcc),
    DEF_TR_FUNC(FCMOVNBE,   fcmovcc),
    DEF_TR_FUNC(FCMOVNB,    fcmovcc),
    DEF_TR_FUNC(FCMOVNE,    fcmovcc),
    DEF_TR_FUNC(FCMOVNU,    fcmovcc),
    DEF_TR_FUNC(FCOMP,  fcomp),
    DEF_TR_FUNC(FCOMIP, fcomip),
    DEF_TR_FUNC(FCOMI,  fcomi),
    DEF_TR_FUNC(FCOM,   fcom),
    DEF_TR_FUNC(FCOS,   fcos),
    DEF_TR_FUNC(FSIN,   fsin),
    DEF_TR_FUNC(FLDZ,   fldz),
    DEF_TR_FUNC(FLD1,   fld1),
    DEF_TR_FUNC(FLD,    fld),
    DEF_TR_FUNC(F2XM1,      f2xm1),
    DEF_TR_FUNC(FBLD,       fbld),
    DEF_TR_FUNC(FBSTP,      fbstp),
    DEF_TR_FUNC(FCOMPP,     fcompp),
    DEF_TR_FUNC(FDECSTP,    fdecstp),
    DEF_TR_FUNC(FFREE,      ffree),
    DEF_TR_FUNC(FICOM,      ficom),
    DEF_TR_FUNC(FICOMP,     ficomp),
    DEF_TR_FUNC(FINCSTP,    fincstp),
    DEF_TR_FUNC(FLDCW,      fldcw),
    DEF_TR_FUNC(FLDENV,     fldenv),
    DEF_TR_FUNC(FLDL2E,     fldl2e),
    DEF_TR_FUNC(FLDL2T,     fldl2t),
    DEF_TR_FUNC(FLDLG2,     fldlg2),
    DEF_TR_FUNC(FLDLN2,     fldln2),
    DEF_TR_FUNC(FLDPI,      fldpi),
    DEF_TR_FUNC(FNCLEX,     fnclex),
    DEF_TR_FUNC(FNINIT,     fninit),
    DEF_TR_FUNC(FNOP,       fnop),
    DEF_TR_FUNC(FNSTCW,     fnstcw),
    DEF_TR_FUNC(FNSTSW,     fnstsw),
    DEF_TR_FUNC(FPATAN,     fpatan),
    DEF_TR_FUNC(FPREM,      fprem),
    DEF_TR_FUNC(FPREM1,     fprem1),
    DEF_TR_FUNC(FPTAN,      fptan),
    DEF_TR_FUNC(FRNDINT,    frndint),
    DEF_TR_FUNC(FRSTOR,     frstor),
    DEF_TR_FUNC(FNSAVE,     fnsave),
    DEF_TR_FUNC(FSCALE,     fscale),
    DEF_TR_FUNC(FSETPM,     fsetpm),
    DEF_TR_FUNC(FSINCOS,    fsincos),
    DEF_TR_FUNC(FNSTENV,    fnstenv),
    DEF_TR_FUNC(FXAM,       fxam),
    DEF_TR_FUNC(FXRSTOR,    fxrstor),
    DEF_TR_FUNC(FXRSTOR64,  fxrstor),
    DEF_TR_FUNC(FXSAVE,     fxsave),
    DEF_TR_FUNC(FXSAVE64,   fxsave),
    DEF_TR_FUNC(FXTRACT,    fxtract),
    DEF_TR_FUNC(FYL2X,      fyl2x),
    DEF_TR_FUNC(FYL2XP1,    fyl2xp1),
    DEF_TR_FUNC(FILD,       fild),
    DEF_TR_FUNC(FDIVR,      fdivr),
    DEF_TR_FUNC(FIDIVR,     fidivr),
    DEF_TR_FUNC(FDIVRP,     fdivrp),
    DEF_TR_FUNC(FDIV,       fdiv),
    DEF_TR_FUNC(FIDIV,      fidiv),
    DEF_TR_FUNC(FDIVP,      fdivp),
    DEF_TR_FUNC(FMUL,       fmul),
    DEF_TR_FUNC(FIMUL,      fimul),
    DEF_TR_FUNC(FMULP,      fmulp),
    DEF_TR_FUNC(FUCOMIP,    fucomip),
    DEF_TR_FUNC(FUCOMI,     fucomi),
    DEF_TR_FUNC(FUCOMPP,    fucompp),
    DEF_TR_FUNC(FUCOMP,     fucomp),
    DEF_TR_FUNC(FUCOM,      fucom),
    DEF_TR_FUNC(FSQRT,      fsqrt),
    DEF_TR_FUNC(FST,        fst),
    DEF_TR_FUNC(FSTP,       fstp),
    DEF_TR_FUNC(FSTPNCE,    fstp),
    DEF_TR_FUNC(FXCH,       fxch),
    DEF_TR_FUNC(FSUBR,      fsubr),
    DEF_TR_FUNC(FISUBR,     fisubr),
    DEF_TR_FUNC(FSUBRP,     fsubrp),
    DEF_TR_FUNC(FSUB,       fsub),
    DEF_TR_FUNC(FISUB,      fisub),
    DEF_TR_FUNC(FSUBP,      fsubp),
    DEF_TR_FUNC(FTST,       ftst),
    DEF_TR_FUNC(PAUSE,  pause),

    DEF_TR_FUNC(CMP,        cmp),
    DEF_TR_FUNC(COMISD,     comisd),
    DEF_TR_FUNC(COMISS,     comiss),
    DEF_TR_FUNC(CMPXCHG,    cmpxchg),
    DEF_TR_FUNC(CMPXCHG8B,  cmpxchg8b),
    DEF_TR_FUNC(CMPXCHG16B, cmpxchg16b),

    DEF_TR_FUNC(CPUID,  cpuid),

    DEF_TR_FUNC(CBW,    cbw),
    DEF_TR_FUNC(CDQ,    cdq),
    DEF_TR_FUNC(CDQE,   cdqe),
    DEF_TR_FUNC(CQO,    cqo),
    DEF_TR_FUNC(CWD,    cwd),
    DEF_TR_FUNC(CWDE,   cwde),

    DEF_TR_FUNC(CVTDQ2PD,   cvtdq2pd),
    DEF_TR_FUNC(CVTDQ2PS,   cvtdq2ps),
    DEF_TR_FUNC(CVTPD2DQ,   cvtpd2dq),
    DEF_TR_FUNC(CVTPD2PS,   cvtpd2ps),
    DEF_TR_FUNC(CVTPS2DQ,   cvtps2dq),
    DEF_TR_FUNC(CVTPS2PD,   cvtps2pd),
    DEF_TR_FUNC(CVTSD2SI,   cvtsd2si),
    DEF_TR_FUNC(CVTSD2SS,   cvtsd2ss),
    DEF_TR_FUNC(CVTSI2SD,   cvtsi2sd),
    DEF_TR_FUNC(CVTSI2SS,   cvtsi2ss),
    DEF_TR_FUNC(CVTSS2SD,   cvtss2sd),
    DEF_TR_FUNC(CVTSS2SI,   cvtss2si),
    DEF_TR_FUNC(CVTTPD2DQ,  cvttpd2dq),
    DEF_TR_FUNC(CVTTPS2DQ,  cvttps2dq),
    DEF_TR_FUNC(CVTTSD2SI,  cvttsd2si),
    DEF_TR_FUNC(CVTTSS2SI,  cvttss2si),
    DEF_TR_FUNC(CVTPD2PI,   cvtpd2pi),
    DEF_TR_FUNC(CVTPI2PD,   cvtpi2pd),
    DEF_TR_FUNC(CVTPI2PS,   cvtpi2ps),
    DEF_TR_FUNC(CVTPS2PI,   cvtps2pi),
    DEF_TR_FUNC(CVTTPD2PI,  cvttpd2pi),
    DEF_TR_FUNC(CVTTPS2PI,  cvttps2pi),

    DEF_TR_FUNC(ENTER,  enter),
    DEF_TR_FUNC(LEAVE,  leave),

    DEF_TR_FUNC(DIV,    div),
    DEF_TR_FUNC(DIVPD,  divpd),
    DEF_TR_FUNC(DIVPS,  divps),
    DEF_TR_FUNC(DIVSD,  divsd),
    DEF_TR_FUNC(DIVSS,  divss),
    DEF_TR_FUNC(IDIV,   idiv),
    DEF_TR_FUNC(MUL,    mul),
    DEF_TR_FUNC(MULPD,  mulpd),
    DEF_TR_FUNC(MULPS,  mulps),
    DEF_TR_FUNC(MULSD,  mulsd),
    DEF_TR_FUNC(MULSS,  mulss),
    DEF_TR_FUNC(IMUL,   imul),

    DEF_TR_FUNC(IRET,   iret),
    DEF_TR_FUNC(IRETD,  iret),
    DEF_TR_FUNC(IRETQ,  iret),

    DEF_TR_FUNC(IN,     in),
    DEF_TR_FUNC(INSB,   ins),
    DEF_TR_FUNC(INSD,   ins),
    DEF_TR_FUNC(INSW,   ins),
    DEF_TR_FUNC(OUT,    out),
    DEF_TR_FUNC(OUTSB,  outs),
    DEF_TR_FUNC(OUTSD,  outs),
    DEF_TR_FUNC(OUTSW,  outs),

    DEF_TR_FUNC(INT,    int),
    DEF_TR_FUNC(INT1,   int1),
    DEF_TR_FUNC(INT3,   int_3),
    DEF_TR_FUNC(INTO,   into),
    DEF_TR_FUNC(FISTTP, fisttp),
    DEF_TR_FUNC(FIST,   fist),
    DEF_TR_FUNC(FISTP,  fistp),
    DEF_TR_FUNC(UCOMISD,    ucomisd),
    DEF_TR_FUNC(UCOMISS,    ucomiss),

    DEF_TR_FUNC(JAE,    jae),
    DEF_TR_FUNC(JA,     ja),
    DEF_TR_FUNC(JBE,    jbe),
    DEF_TR_FUNC(JB,     jb),
    DEF_TR_FUNC(JCXZ,   jcxz),
    DEF_TR_FUNC(JECXZ,  jecxz),
    DEF_TR_FUNC(JE,     jz),
    DEF_TR_FUNC(JGE,    jge),
    DEF_TR_FUNC(JG,     jg),
    DEF_TR_FUNC(JLE,    jle),
    DEF_TR_FUNC(JL,     jl),
    DEF_TR_FUNC(JNE,    jnz),
    DEF_TR_FUNC(JNO,    jno),
    DEF_TR_FUNC(JNP,    jnp),
    DEF_TR_FUNC(JNS,    jns),
    DEF_TR_FUNC(JO,     jo),
    DEF_TR_FUNC(JP,     jp),
    DEF_TR_FUNC(JRCXZ,  jrcxz),
    DEF_TR_FUNC(JS,     js),
    DEF_TR_FUNC(LOOP,   loop),
    DEF_TR_FUNC(LOOPE,  loopz),
    DEF_TR_FUNC(LOOPNE, loopnz),

    DEF_TR_FUNC(LEA,    lea),

    DEF_TR_FUNC(LDMXCSR,    ldmxcsr),
    DEF_TR_FUNC(STMXCSR,    stmxcsr),

    DEF_TR_FUNC(LFENCE, lfence),
    DEF_TR_FUNC(SFENCE, sfence),
    DEF_TR_FUNC(MFENCE, mfence),

    DEF_TR_FUNC(LDS,    lds),
    DEF_TR_FUNC(LES,    les),
    DEF_TR_FUNC(LFS,    lfs),
    DEF_TR_FUNC(LGS,    lgs),
    DEF_TR_FUNC(LSS,    lss),

    DEF_TR_FUNC(LAHF,   lahf),
    DEF_TR_FUNC(SAHF,   sahf),

    DEF_TR_FUNC(EMMS,   emms),
    DEF_TR_FUNC(XADD,   xadd),

    DEF_TR_FUNC(OR,     or),
    DEF_TR_FUNC(ORPD,   orpd),
    DEF_TR_FUNC(ORPS,   orps),

    DEF_TR_FUNC(SUB,    sub),
    DEF_TR_FUNC(SUBSD,  subsd),
    DEF_TR_FUNC(SUBSS,  subss),
    DEF_TR_FUNC(SUBPD,  subpd),
    DEF_TR_FUNC(SUBPS,  subps),

    DEF_TR_FUNC(XOR,    xor),
    DEF_TR_FUNC(XORPD,  xorpd),
    DEF_TR_FUNC(XORPS,  xorps),

    DEF_TR_FUNC(MASKMOVDQU, maskmovdqu),
    DEF_TR_FUNC(MASKMOVQ,   maskmovq),

    DEF_TR_FUNC(MAXPD,  maxpd),
    DEF_TR_FUNC(MAXPS,  maxps),
    DEF_TR_FUNC(MAXSD,  maxsd),
    DEF_TR_FUNC(MAXSS,  maxss),
    DEF_TR_FUNC(MINPD,  minpd),
    DEF_TR_FUNC(MINPS,  minps),
    DEF_TR_FUNC(MINSD,  minsd),
    DEF_TR_FUNC(MINSS,  minss),

    DEF_TR_FUNC(HADDPD,   haddpd),
    DEF_TR_FUNC(HADDPS,   haddps),

    DEF_TR_FUNC(PACKSSDW,   packssdw),
    DEF_TR_FUNC(PACKSSWB,   packsswb),
    DEF_TR_FUNC(PACKUSWB,   packuswb),
    DEF_TR_FUNC(PADDB,      paddb),
    DEF_TR_FUNC(PADDD,      paddd),
    DEF_TR_FUNC(PADDQ,      paddq),
    DEF_TR_FUNC(PADDSB,     paddsb),
    DEF_TR_FUNC(PADDSW,     paddsw),
    DEF_TR_FUNC(PADDUSB,    paddusb),
    DEF_TR_FUNC(PADDUSW,    paddusw),
    DEF_TR_FUNC(PADDW,      paddw),
    DEF_TR_FUNC(PANDN,      pandn),
    DEF_TR_FUNC(PAND,       pand),
    DEF_TR_FUNC(PAVGB,      pavgb),
    DEF_TR_FUNC(PAVGW,      pavgw),
    DEF_TR_FUNC(PCMPEQB,    pcmpeqb),
    DEF_TR_FUNC(PCMPEQD,    pcmpeqd),
    DEF_TR_FUNC(PCMPEQW,    pcmpeqw),
    DEF_TR_FUNC(PCMPGTB,    pcmpgtb),
    DEF_TR_FUNC(PCMPGTD,    pcmpgtd),
    DEF_TR_FUNC(PCMPGTW,    pcmpgtw),
    DEF_TR_FUNC(PEXTRW,     pextrw),
    DEF_TR_FUNC(PINSRW,     pinsrw),
    DEF_TR_FUNC(PMADDWD,    pmaddwd),
    DEF_TR_FUNC(PMAXSW,     pmaxsw),
    DEF_TR_FUNC(PMAXUB,     pmaxub),
    DEF_TR_FUNC(PMINSW,     pminsw),
    DEF_TR_FUNC(PMINUB,     pminub),
    DEF_TR_FUNC(PMOVMSKB,   pmovmskb),
    DEF_TR_FUNC(PMULHUW,    pmulhuw),
    DEF_TR_FUNC(PMULHW,     pmulhw),
    DEF_TR_FUNC(PMULLW,     pmullw),
    DEF_TR_FUNC(PMULUDQ,    pmuludq),
    DEF_TR_FUNC(POR,        por),
    DEF_TR_FUNC(PSADBW,     psadbw),
    DEF_TR_FUNC(PSHUFW,     pshufw),
    DEF_TR_FUNC(PSLLD,      pslld),
    DEF_TR_FUNC(PSLLQ,      psllq),
    DEF_TR_FUNC(PSLLW,      psllw),
    DEF_TR_FUNC(PSRAD,      psrad),
    DEF_TR_FUNC(PSRAW,      psraw),
    DEF_TR_FUNC(PSRLD,      psrld),
    DEF_TR_FUNC(PSRLQ,      psrlq),
    DEF_TR_FUNC(PSRLW,      psrlw),
    DEF_TR_FUNC(PSUBB,      psubb),
    DEF_TR_FUNC(PSUBD,      psubd),
    DEF_TR_FUNC(PSUBQ,      psubq),
    DEF_TR_FUNC(PSUBSB,     psubsb),
    DEF_TR_FUNC(PSUBSW,     psubsw),
    DEF_TR_FUNC(PSUBUSB,    psubusb),
    DEF_TR_FUNC(PSUBUSW,    psubusw),
    DEF_TR_FUNC(PSUBW,      psubw),
    DEF_TR_FUNC(PUNPCKHBW,  punpckhbw),
    DEF_TR_FUNC(PUNPCKHDQ,  punpckhdq),
    DEF_TR_FUNC(PUNPCKHWD,  punpckhwd),
    DEF_TR_FUNC(PUNPCKLBW,  punpcklbw),
    DEF_TR_FUNC(PUNPCKLDQ,  punpckldq),
    DEF_TR_FUNC(PUNPCKLWD,  punpcklwd),
    DEF_TR_FUNC(PXOR,       pxor),
    DEF_TR_FUNC(PSHUFD,     pshufd),
    DEF_TR_FUNC(PSHUFHW,    pshufhw),
    DEF_TR_FUNC(PSHUFLW,    pshuflw),
    DEF_TR_FUNC(PSLLDQ,     pslldq),
    DEF_TR_FUNC(PSRLDQ,     psrldq),
    DEF_TR_FUNC(PUNPCKHQDQ, punpckhqdq),
    DEF_TR_FUNC(PUNPCKLQDQ, punpcklqdq),
    DEF_TR_FUNC(RCPPS,      rcpps),
    DEF_TR_FUNC(RCPSS,      rcpss),
    DEF_TR_FUNC(RSQRTPS,    rsqrtps),
    DEF_TR_FUNC(RSQRTSS,    rsqrtss),
    DEF_TR_FUNC(SHUFPD,     shufpd),
    DEF_TR_FUNC(SHUFPS,     shufps),
    DEF_TR_FUNC(SQRTPD,     sqrtpd),
    DEF_TR_FUNC(SQRTPS,     sqrtps),
    DEF_TR_FUNC(SQRTSD,     sqrtsd),
    DEF_TR_FUNC(SQRTSS,     sqrtss),
    DEF_TR_FUNC(UNPCKHPD,   unpckhpd),
    DEF_TR_FUNC(UNPCKHPS,   unpckhps),
    DEF_TR_FUNC(UNPCKLPD,   unpcklpd),
    DEF_TR_FUNC(UNPCKLPS,   unpcklps),
    DEF_TR_FUNC(CMPEQSS,    cmpeqss),
    DEF_TR_FUNC(CMPEQSD,    cmpeqsd),
    DEF_TR_FUNC(CMPEQPS,    cmpeqps),
    DEF_TR_FUNC(CMPEQPD,    cmpeqpd),
    DEF_TR_FUNC(CMPLTSS,    cmpltss),
    DEF_TR_FUNC(CMPLTSD,    cmpltsd),
    DEF_TR_FUNC(CMPLTPS,    cmpltps),
    DEF_TR_FUNC(CMPLTPD,    cmpltpd),
    DEF_TR_FUNC(CMPLESS,    cmpless),
    DEF_TR_FUNC(CMPLESD,    cmplesd),
    DEF_TR_FUNC(CMPLEPS,    cmpleps),
    DEF_TR_FUNC(CMPLEPD,    cmplepd),
    DEF_TR_FUNC(CMPNEQSS,   cmpneqss),
    DEF_TR_FUNC(CMPNEQSD,   cmpneqsd),
    DEF_TR_FUNC(CMPNEQPS,   cmpneqps),
    DEF_TR_FUNC(CMPNEQPD,   cmpneqpd),
    DEF_TR_FUNC(CMPNLTSS,   cmpnltss),
    DEF_TR_FUNC(CMPNLTSD,   cmpnltsd),
    DEF_TR_FUNC(CMPNLTPS,   cmpnltps),
    DEF_TR_FUNC(CMPNLTPD,   cmpnltpd),
    DEF_TR_FUNC(CMPNLESS,   cmpnless),
    DEF_TR_FUNC(CMPNLESD,   cmpnlesd),
    DEF_TR_FUNC(CMPNLEPS,   cmpnleps),
    DEF_TR_FUNC(CMPNLEPD,   cmpnlepd),
    DEF_TR_FUNC(CMPORDSS,   cmpordss),
    DEF_TR_FUNC(CMPORDSD,   cmpordsd),
    DEF_TR_FUNC(CMPORDPS,   cmpordps),
    DEF_TR_FUNC(CMPORDPD,   cmpordpd),
    DEF_TR_FUNC(CMPUNORDSS, cmpunordss),
    DEF_TR_FUNC(CMPUNORDSD, cmpunordsd),
    DEF_TR_FUNC(CMPUNORDPS, cmpunordps),
    DEF_TR_FUNC(CMPUNORDPD, cmpunordpd),

    DEF_TR_FUNC(MOV,        mov),
    DEF_TR_FUNC(MOVABS,     mov),
    DEF_TR_FUNC(MOVDQA,     movdqa),
    DEF_TR_FUNC(MOVDQU,     movdqu),
    DEF_TR_FUNC(MOVHPD,     movhpd),
    DEF_TR_FUNC(MOVHPS,     movhps),
    DEF_TR_FUNC(MOVLPD,     movlpd),
    DEF_TR_FUNC(MOVLPS,     movlps),
    DEF_TR_FUNC(MOVHLPS,    movhlps),
    DEF_TR_FUNC(MOVLHPS,    movlhps),
    DEF_TR_FUNC(MOVMSKPD,   movmskpd),
    DEF_TR_FUNC(MOVMSKPS,   movmskps),
    DEF_TR_FUNC(MOVNTDQ,    movntdq),
    DEF_TR_FUNC(MOVNTI,     movnti),
    DEF_TR_FUNC(MOVNTPD,    movntpd),
    DEF_TR_FUNC(MOVNTPS,    movntps),
    DEF_TR_FUNC(MOVSS,      movss),
    DEF_TR_FUNC(MOVSXD,     movsxd),
    DEF_TR_FUNC(MOVUPD,     movupd),
    DEF_TR_FUNC(MOVUPS,     movups),
    DEF_TR_FUNC(MOVZX,      movzx),
    DEF_TR_FUNC(MOVSX,      movsx),
    DEF_TR_FUNC(MOVD,       movd),
    DEF_TR_FUNC(MOVDQ2Q,    movdq2q),
    DEF_TR_FUNC(MOVNTQ,     movntq),
    DEF_TR_FUNC(MOVQ2DQ,    movq2dq),
    DEF_TR_FUNC(MOVQ,       movq),
    DEF_TR_FUNC(MOVAPD,     movapd),
    DEF_TR_FUNC(MOVAPS,     movaps),

    DEF_TR_FUNC(NEG,    neg),
    DEF_TR_FUNC(NOT,    not),
    DEF_TR_FUNC(NOP,    nop),

    DEF_TR_FUNC(POP,    pop),
    DEF_TR_FUNC(POPAW,  popaw),
    DEF_TR_FUNC(POPAL,  popal),
    DEF_TR_FUNC(POPF,   popf),
    DEF_TR_FUNC(POPFD,  popf),
    DEF_TR_FUNC(POPFQ,  popf),
    DEF_TR_FUNC(PUSH,   push),
    DEF_TR_FUNC(PUSHAW, pushaw),
    DEF_TR_FUNC(PUSHAL, pushal),
    DEF_TR_FUNC(PUSHF,  pushf),
    DEF_TR_FUNC(PUSHFD, pushf),
    DEF_TR_FUNC(PUSHFQ, pushf),

    DEF_TR_FUNC(PREFETCH,       prefetch),
    DEF_TR_FUNC(PREFETCHNTA,    prefetchnta),
    DEF_TR_FUNC(PREFETCHT0,     prefetcht0),
    DEF_TR_FUNC(PREFETCHT1,     prefetcht1),
    DEF_TR_FUNC(PREFETCHT2,     prefetcht2),
    DEF_TR_FUNC(PREFETCHW,      prefetchw),

    DEF_TR_FUNC(RCL,    rcl),
    DEF_TR_FUNC(RCR,    rcr),
    DEF_TR_FUNC(ROL,    rol),
    DEF_TR_FUNC(ROR,    ror),

    DEF_TR_FUNC(SAL,    sal),
    DEF_TR_FUNC(SAR,    sar),
    DEF_TR_FUNC(SARX,   sarx),
    DEF_TR_FUNC(SHL,    shl),
    DEF_TR_FUNC(SHLD,   shld),
    DEF_TR_FUNC(SHLX,   shlx),
    DEF_TR_FUNC(SHR,    shr),
    DEF_TR_FUNC(SHRD,   shrd),
    DEF_TR_FUNC(SHRX,   shrx),

    DEF_TR_FUNC(SCASB,  scas),
    DEF_TR_FUNC(SCASD,  scas),
    DEF_TR_FUNC(SCASQ,  scas),
    DEF_TR_FUNC(SCASW,  scas),
    DEF_TR_FUNC(STOSB,  stos),
    DEF_TR_FUNC(STOSD,  stos),
    DEF_TR_FUNC(STOSQ,  stos),
    DEF_TR_FUNC(STOSW,  stos),
    DEF_TR_FUNC(LODSB,  lods),
    DEF_TR_FUNC(LODSD,  lods),
    DEF_TR_FUNC(LODSQ,  lods),
    DEF_TR_FUNC(LODSW,  lods),
    DEF_TR_FUNC(MOVSB,  movs),
    DEF_TR_FUNC(MOVSQ,  movs),
    DEF_TR_FUNC(MOVSW,  movs),
    DEF_TR_FUNC(CMPSB,  cmps),
    DEF_TR_FUNC(CMPSD,  cmps),
    DEF_TR_FUNC(CMPSQ,  cmps),
    DEF_TR_FUNC(CMPSW,  cmps),

    DEF_TR_FUNC(SETAE,  setcc),
    DEF_TR_FUNC(SETA,   setcc),
    DEF_TR_FUNC(SETBE,  setcc),
    DEF_TR_FUNC(SETB,   setcc),
    DEF_TR_FUNC(SETE,   setcc),
    DEF_TR_FUNC(SETGE,  setcc),
    DEF_TR_FUNC(SETG,   setcc),
    DEF_TR_FUNC(SETLE,  setcc),
    DEF_TR_FUNC(SETL,   setcc),
    DEF_TR_FUNC(SETNE,  setcc),
    DEF_TR_FUNC(SETNO,  setcc),
    DEF_TR_FUNC(SETNP,  setcc),
    DEF_TR_FUNC(SETNS,  setcc),
    DEF_TR_FUNC(SETO,   setcc),
    DEF_TR_FUNC(SETP,   setcc),
    DEF_TR_FUNC(SETS,   setcc),

    DEF_TR_FUNC(SYSCALL,    syscall),
    DEF_TR_FUNC(SYSENTER,   sysenter),
    DEF_TR_FUNC(SYSEXIT,    sysexit),
    DEF_TR_FUNC(SYSRET,     sysret),

    DEF_TR_FUNC(TEST,   test),
    DEF_TR_FUNC(TZCNT,  bsf),
    DEF_TR_FUNC(WAIT,   wait),
    DEF_TR_FUNC(XCHG,   xchg),
    DEF_TR_FUNC(XLATB,   xlat),

    DEF_TR_FUNC(XSAVE,      xsave),
    DEF_TR_FUNC(XSAVEOPT,   xsaveopt),
    DEF_TR_FUNC(XRSTOR,     xrstor),

    DEF_TR_FUNC(SWAPGS, swapgs),

    DEF_TR_FUNC(UD2,    ud2),
    DEF_TR_FUNC(UD0,    ud0),

    DEF_TR_FUNC(ENDBR32,   to_null),
    DEF_TR_FUNC(ENDBR64,   to_null),

    /* privilege instructions */
    DEF_TR_FUNC(LLDT,   lldt),
    DEF_TR_FUNC(SLDT,   sldt),
    DEF_TR_FUNC(LGDT,   lgdt),
    DEF_TR_FUNC(SGDT,   sgdt),
    DEF_TR_FUNC(LTR,    ltr),
    DEF_TR_FUNC(STR,    str),
    DEF_TR_FUNC(LIDT,   lidt),
    DEF_TR_FUNC(SIDT,   sidt),
    DEF_TR_FUNC(LMSW,   lmsw),
    DEF_TR_FUNC(SMSW,   smsw),
    DEF_TR_FUNC(CLTS,   clts),
    DEF_TR_FUNC(ARPL, arpl),
    /* LAR */
    DEF_TR_FUNC(LSL,    lsl),
    DEF_TR_FUNC(LAR,    lar),
    DEF_TR_FUNC(VERR,   verr),
    DEF_TR_FUNC(VERW,   verw),
    DEF_TR_FUNC(INVD,   invd),
    DEF_TR_FUNC(WBINVD, wbinvd),
    DEF_TR_FUNC(INVPCID, invpcid),
    DEF_TR_FUNC(INVLPG, invlpg),
    DEF_TR_FUNC(INVLPGA,    invlpga),
    DEF_TR_FUNC(HLT,    hlt),
    DEF_TR_FUNC(RSM,    rsm),
    DEF_TR_FUNC(RDMSR,  rdmsr),
    DEF_TR_FUNC(WRMSR,  wrmsr),
    DEF_TR_FUNC(RDPMC,  rdpmc),
    DEF_TR_FUNC(RDTSC,  rdtsc),
    DEF_TR_FUNC(RDTSCP, rdtscp),

    DEF_TR_FUNC(XGETBV, xgetbv),
    DEF_TR_FUNC(XSETBV, xsetbv),

    DEF_TR_FUNC(EXTRACTPS, extractps),
    /*DEF_TR_FUNC(EXTRQ, extrq),*/
    /*DEF_TR_FUNC(FEMMS, femms),*/
    DEF_TR_FUNC(FFREEP, ffreep),
    /*DEF_TR_FUNC(VMOVAPD, vmovapd),*/
    /*DEF_TR_FUNC(VMOVAPS, vmovaps),*/
    /*DEF_TR_FUNC(GETSEC, getsec),*/
    DEF_TR_FUNC(HADDPD, haddpd),
    DEF_TR_FUNC(HADDPS, haddps),
    DEF_TR_FUNC(HSUBPD, hsubpd),
    DEF_TR_FUNC(HSUBPS, hsubps),
    DEF_TR_FUNC(INSERTPS, insertps),
    /*DEF_TR_FUNC(INSERTQ, insertq),*/
    /*DEF_TR_FUNC(INVEPT, invept),*/
    DEF_TR_FUNC(RDRAND, rdrand),

    DEF_TR_FUNC(CLFLUSH, clflush),

    /* ssse3 */
    DEF_TR_FUNC(PSHUFB,  pshufb),
    DEF_TR_FUNC(PALIGNR, palignr),
    DEF_TR_FUNC(PSIGNB,  psignb),
    DEF_TR_FUNC(PSIGNW,  psignw),
    DEF_TR_FUNC(PSIGND,  psignd),
    DEF_TR_FUNC(PHADDD,   phaddd),
    DEF_TR_FUNC(PHADDW,   phaddw),
    DEF_TR_FUNC(PHADDSW,  phaddsw),
    DEF_TR_FUNC(PHSUBD,   phsubd),
    DEF_TR_FUNC(PHSUBW,   phsubw),
    DEF_TR_FUNC(PHSUBSW,  phsubsw),
    DEF_TR_FUNC(MOVSHDUP, movshdup),
    DEF_TR_FUNC(MOVSLDUP, movsldup),
    DEF_TR_FUNC(MOVDDUP,  movddup),
    DEF_TR_FUNC(PABSB      ,pabsb),
    DEF_TR_FUNC(PABSW      ,pabsw),
    DEF_TR_FUNC(PABSD      ,pabsd),
    DEF_TR_FUNC(PMADDUBSW  ,pmaddubsw),
    DEF_TR_FUNC(ADDSUBPS   ,addsubps),
    DEF_TR_FUNC(ADDSUBPD   ,addsubpd),
    DEF_TR_FUNC(PMULHRSW   ,pmulhrsw),


    /* sse41 */
    DEF_TR_FUNC(PTEST, ptest),
    DEF_TR_FUNC(PEXTRQ, pextrq),
    DEF_TR_FUNC(PEXTRD, pextrd),
    DEF_TR_FUNC(PEXTRB, pextrb),
    DEF_TR_FUNC(PINSRB, pinsrb),
    DEF_TR_FUNC(PINSRD, pinsrd),
    DEF_TR_FUNC(PINSRQ, pinsrq),
    DEF_TR_FUNC(PMOVZXBQ, pmovzxbq),
    DEF_TR_FUNC(PMOVZXDQ, pmovzxdq),
    DEF_TR_FUNC(PMOVZXBW, pmovzxbw),
    DEF_TR_FUNC(PMOVZXBD, pmovzxbd),
    DEF_TR_FUNC(PMOVZXWD, pmovzxwd),
    DEF_TR_FUNC(PMOVZXWQ, pmovzxwq),
    DEF_TR_FUNC(PMOVSXBW, pmovsxbw),
    DEF_TR_FUNC(PMOVSXDQ, pmovsxdq),
    DEF_TR_FUNC(PMOVSXBD, pmovsxbd),
    DEF_TR_FUNC(PMOVSXWD, pmovsxwd),
    DEF_TR_FUNC(PMOVSXBQ, pmovsxbq),
    DEF_TR_FUNC(PMOVSXWQ, pmovsxwq),
    DEF_TR_FUNC(BLENDVPD, blendvpd),
    DEF_TR_FUNC(BLENDVPS, blendvps),
    DEF_TR_FUNC(ROUNDSS, roundss),
    DEF_TR_FUNC(ROUNDPS, roundps),
    DEF_TR_FUNC(ROUNDSD, roundsd),
    DEF_TR_FUNC(ROUNDPD, roundpd),
    DEF_TR_FUNC(PCMPEQQ, pcmpeqq),
    DEF_TR_FUNC(PBLENDW, pblendw),
    DEF_TR_FUNC(PBLENDVB, pblendvb),
    DEF_TR_FUNC(INSERTPS, insertps),
    DEF_TR_FUNC(EXTRACTPS, extractps),
    DEF_TR_FUNC(PMINUD, pminud),
    DEF_TR_FUNC(PMINSD, pminsd),
    DEF_TR_FUNC(PMINUW, pminuw),
    DEF_TR_FUNC(PMINSB, pminsb),
    DEF_TR_FUNC(PMULLD, pmulld),
    DEF_TR_FUNC(PMULDQ, pmuldq),
    DEF_TR_FUNC(PMAXSD, pmaxsd),
    DEF_TR_FUNC(PMAXUD, pmaxud),
    DEF_TR_FUNC(PMAXSB, pmaxsb),
    DEF_TR_FUNC(PMAXUW, pmaxuw),
    DEF_TR_FUNC(PACKUSDW, packusdw),
    DEF_TR_FUNC(DPPD, dppd),
    DEF_TR_FUNC(DPPS, dpps),
    DEF_TR_FUNC(MOVNTDQA, movntdqa),
    DEF_TR_FUNC(BLENDPD, blendpd),
    DEF_TR_FUNC(BLENDPS, blendps),

    /* sse42 */
    DEF_TR_FUNC(PCMPGTQ, pcmpgtq),
    DEF_TR_FUNC(PCMPISTRI, pcmpistri),
    DEF_TR_FUNC(PCMPESTRI, pcmpestri),
    DEF_TR_FUNC(PCMPISTRM, pcmpistrm),
    DEF_TR_FUNC(PCMPESTRM, pcmpestrm),

    DEF_TR_FUNC(CRC32, crc32),
    DEF_TR_FUNC(POPCNT, popcnt),

    DEF_TR_FUNC(ADCX, adcx),
    DEF_TR_FUNC(ADOX, adox),
    DEF_TR_FUNC(AESDEC, aesdec),
    DEF_TR_FUNC(AESDECLAST, aesdeclast),
    DEF_TR_FUNC(AESENC, aesenc),
    DEF_TR_FUNC(AESENCLAST, aesenclast),
    DEF_TR_FUNC(AESIMC, aesimc),
    DEF_TR_FUNC(AESKEYGENASSIST, aeskeygenassist),
    DEF_TR_FUNC(ANDN, andn),
//    DEF_TR_FUNC(ARPL, nop),
    DEF_TR_FUNC(BEXTR, bextr),
    DEF_TR_FUNC(BZHI, bzhi),
    DEF_TR_FUNC(BLSI, blsi),
    DEF_TR_FUNC(BLSMSK, blsmsk),
    DEF_TR_FUNC(BLSR, blsr),
    DEF_TR_FUNC(BOUND, nop),
    DEF_TR_FUNC(CLFLUSHOPT, clflushopt),
    DEF_TR_FUNC(CMPPD, cmppd),
    DEF_TR_FUNC(CMPPS, cmpps),
    DEF_TR_FUNC(CMPSS, cmpss),
    DEF_TR_FUNC(LDDQU, lddqu),
    DEF_TR_FUNC(LZCNT, lzcnt),
    DEF_TR_FUNC(MPSADBW, mpsadbw),
    DEF_TR_FUNC(MULX, mulx),
    DEF_TR_FUNC(MOVBE, movbe),
    DEF_TR_FUNC(PCLMULQDQ, pclmulqdq),
    DEF_TR_FUNC(PDEP, pdep),
    DEF_TR_FUNC(PEXT, pext),
    DEF_TR_FUNC(PHMINPOSUW, phminposuw),
    DEF_TR_FUNC(RORX, rorx),
    DEF_TR_FUNC(SALC, salc),
    DEF_TR_FUNC(SHA1MSG1, sha1msg1),
    DEF_TR_FUNC(SHA1MSG2, sha1msg2),
    DEF_TR_FUNC(SHA1NEXTE, sha1nexte),
    DEF_TR_FUNC(SHA1RNDS4, sha1rnds4),
    DEF_TR_FUNC(SHA256MSG1, sha256msg1),
    DEF_TR_FUNC(SHA256MSG2, sha256msg2),
    DEF_TR_FUNC(SHA256RNDS2, sha256rnds2),

    /* X86_INS_ENDING */
};

bool ir1_translate(IR1_INST *ir1)
{
#ifdef LATXS_INSTPTN_ENABLE
    if (try_translate_instptn(ir1)) {
        return true;
    }
#endif

    /* 2. call translate_xx function */
    int tr_func_idx = ir1_opcode(ir1) - X86_INS_INVALID;

    bool translation_success = false;

    // MOVSD means movsd(movs) or movsd(sse2) , diff opcode
    if (ir1_opcode(ir1) == X86_INS_MOVSD) {
        if (ir1->info->detail->x86.opcode[0] == 0xa5) {
            translation_success = latxs_translate_movs(ir1);
        } else if (ir1->info->detail->x86.opcode[0] == 0x0f) {
            translation_success = latxs_translate_movsd(ir1);
        } else {
#ifdef CONFIG_LATX_PRINT
            fprintf(stderr, "%s %s %d error : this ins not implemented\n",
                    __FILE__, __func__, __LINE__);
            ir1_opcode_dump(ir1);
#endif
            exit(-1);
        }
    } else {
        if (translate_functions[tr_func_idx] == NULL || !latxs_is_ir1_ok(ir1)) {
#ifdef CONFIG_LATX_PRINT
            fprintf(stderr, "%s %s %d error : this ins %d not implemented\n",
                    __FILE__, __func__, __LINE__, tr_func_idx);
            ir1_opcode_dump(ir1);
#endif
#ifdef CONFIG_SOFTMMU
            latxs_tr_gen_excp_illegal_op_addr(ir1_addr(ir1) - lsenv->tr_data->sys.cs_base, 1);
            translation_success = true;
#else
            translation_success = false;
#endif
        } else {
            translation_success = translate_functions[tr_func_idx](ir1); /* TODO */
        }
    }

    if (!translation_success) {
        ir1_dump(ir1);
    }

    lsassertm(translation_success,
            "tr_func_idx %d translate failed, the failed ins is at line %d",
            tr_func_idx,
            (tr_func_idx + idx_start));

    return translation_success;
}

#include "qemu/cacheflush.h"

#ifdef CONFIG_LATX_TU
#define LA_REG_A0 4
#define LA_LU12I_W_A0_ZERO (0x14000000 | LA_REG_A0)
#define LA_ORI_A0_A0_ZERO  (0x03800000 | (LA_REG_A0 << 5) | LA_REG_A0)

static uint32_t latxs_tu_generation_offset_lu12i(uint32_t offset)
{
    return 0x14000000 | ((offset >> 12) << 5) | LA_REG_A0;
}

static uint32_t latxs_tu_generation_offset_ori(uint32_t offset)
{
    return 0x03800000 | ((offset & 0xfff) << 10) |
           (LA_REG_A0 << 5) | LA_REG_A0;
}

static void latxs_tu_generation_offset_patch(TranslationBlock *tb, int n,
                                              uint32_t offset)
{
    uint16_t code_offset = tb->s_data->tu_generation_offset_patch[n];

    if (code_offset == TB_JMP_OFFSET_INVALID) {
        return;
    }

    uint32_t *patch = (uint32_t *)((uintptr_t)tb->tc.ptr + code_offset);

    g_assert(offset <= INT32_MAX);
    qatomic_set(patch, latxs_tu_generation_offset_lu12i(offset));
    qatomic_set(patch + 1, latxs_tu_generation_offset_ori(offset));
    flush_idcache_range((uintptr_t)patch, (uintptr_t)patch, 8);
}

static void latxs_tu_generation_offset_link(
        TranslationBlock *tb, int n, const TranslationBlock *next_tb)
{
    uint32_t offset = TB_TU_PAGES(next_tb) ? TB_TU_PAGES(next_tb)->id : 0;

    if (tucp_fast_link_check()) {
        TB_NEXT_TU_PAGES(tb, n) = TB_TU_PAGES(next_tb);
        TB_NEXT_TU_GENERATION_OFFSET(tb, n) =
            (uint64_t)offset * tucp_generation_entry_size();
    }

    if (tucp_generation_offset()) {
        latxs_tu_generation_offset_patch(tb, n, offset);
    }
}

static void latxs_tu_generation_offset_unlink(TranslationBlock *tb, int n)
{
    if (tucp_fast_link_check()) {
        TB_NEXT_TU_PAGES(tb, n) = NULL;
        TB_NEXT_TU_GENERATION_OFFSET(tb, n) = 0;
    }

    if (tucp_generation_offset() &&
        tb->s_data->tu_generation_offset_patch[n] != TB_JMP_OFFSET_INVALID) {
        uint16_t code_offset = tb->s_data->tu_generation_offset_patch[n];
        uint32_t *patch = (uint32_t *)((uintptr_t)tb->tc.ptr + code_offset);

        qatomic_set(patch, LA_LU12I_W_A0_ZERO);
        qatomic_set(patch + 1, LA_ORI_A0_A0_ZERO);
        flush_idcache_range((uintptr_t)patch, (uintptr_t)patch, 8);
    }
}

static bool latxs_tucp_pages_cover(const TranslationUnitPages *source,
                                   const TranslationUnitPages *target)
{
    for (int target_index = 0;
         target_index < TRANSLATION_UNIT_MAX_PAGES; target_index++) {
        uint64_t target_mix = target->pages[target_index].mix;
        uint64_t target_vp_phys_hi = target->pages[target_index].vp_phys_hi;
        bool found = false;

        if (target_vp_phys_hi == UINT64_MAX) {
            return true;
        }
        for (int source_index = 0;
             source_index < TRANSLATION_UNIT_MAX_PAGES; source_index++) {
            uint64_t source_mix = source->pages[source_index].mix;
            uint64_t source_vp_phys_hi = source->pages[source_index].vp_phys_hi;

            if (source_vp_phys_hi == UINT64_MAX) {
                break;
            }
            if (source_mix == target_mix &&
                source_vp_phys_hi == target_vp_phys_hi) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return false;
}

static bool latxs_tucp_link_can_bypass_check(const TranslationBlock *source,
                                              const TranslationBlock *target)
{
    return tucp_fast_direct_check() &&
           (source->flags & HF_CPL_MASK) == HF_CPL_MASK &&
           (target->flags & HF_CPL_MASK) == HF_CPL_MASK &&
           tb_is_tu(source) && tb_is_tu(target) &&
           tb_tu_identity(source) != tb_tu_identity(target) &&
           TB_TU_PAGES(source) && TB_TU_PAGES(target) &&
           latxs_tucp_pages_cover(TB_TU_PAGES(source), TB_TU_PAGES(target));
}
#endif

static void __latxs_tb_set_jmp_target(TranslationBlock *tb,
        int n, void *target_ptr, bool use_cp_label)
{
    void *jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
                                  tb->jmp_insn_offset[n]);

    if (use_cp_label && (tb->jmp_cp_data[n] & TB_JMP_CP_DATA_VALID))
    {
        /*
         * +--------+  <--  jmp_cp_data.off
         * |        |
         * |        |  cp_tb_lookup() : save eip
         * |        |  cp_tb_check() : load tb ptr
         * |        |
         * +--------+  <--  jmp_insn_offset
         * |  b 2   |
         * |  nop   |
         * +--------+  <--  jmp_reset_offset
         */
        int jmp_cp_off = tb->jmp_cp_data[n] & TB_JMP_CP_DATA_OFF;
        jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
                (tb->jmp_insn_offset[n] - (jmp_cp_off << 2)));
        tb->jmp_cp_data[n] |= TB_JMP_CP_DATA_LINKED;
        tb->jmp_inst_hold[n] = qatomic_read((uint64_t*)jmp_inst_ptr);
    }
    if (tb->link_opt[n] == 1) {
        tb->link_opt[n] = 2;
        tb->jmp_inst_hold[n] = qatomic_read((uint64_t*)jmp_inst_ptr);
    }
    if (tb->jmp_inst_hold[n] == 0) {
	uint64_t inst_b2  = 0x50000800;
	uint64_t inst_nop = 0x03400000;
	tb->jmp_inst_hold[n] = (inst_nop << 32) | inst_b2;
    }
    int64_t offset = target_ptr - jmp_inst_ptr;

#if 0
    printf("tb %p n %d target ptr %p\n", tb, n, target_ptr);
#endif

    uint32_t inst1, inst2;
    uint32_t *p = jmp_inst_ptr;
    /*
     * IF next TB is not a cross-page TB
     * BUT current TB generates extra codes for it
     *
     * +-------+            +-------+  <-- jmp_inst_ptr - (jmp_cp_off << 2)
     * | inst1 |    link    | link  |
     * | inst2 |   >>>>>>   | codes |  <== link here to override
     * |       |            |       |
     * +-------+            +-------+  <-- jmp_inst_ptr
     * | b 2   |   unlink   | b 2   |
     * | nop   |   <<<<<<   | nop   |
     * +-------+            +-------+
     *
     * The link codes could be one inst or two inst
     * depending on the jump distance.
     */

    offset >>= 2;

    if (offset == sextract64(offset, 0, 26)) {
        /* 256MB */
        /*
         * 24   20   16   12    8    4    0
         *  |    |    |    |    |    |    |
         * ## #### #### #### #### #### ####
         * | to 0:9   |    to 10:25       |
         * 25         16                  0
         * |  0x3ff   |    0x3fffc00      |
         */
        inst1 = ((offset >> 16) & 0x3ff)     |
                ((offset << 10) & 0x3fffc00) | 0x50000000;
        qatomic_set(p, inst1);
        flush_idcache_range((uintptr_t)p, (uintptr_t)p, 4);
    } else if (offset == sextract64(offset, 0, 36)) {
        /* 256GB */
        /*
         *   32   28   24   20   16   12    8    4    0
         *    |    |    |    |    |    |    |    |    |
         * #### #### #### #### #### #### #### #### ####
         * |  pcaddu18i 5:24      |   jirl 0:15       |
         * 35                    16                   0
         */
        int64_t upper = ((offset + (1 << 15)) >> 16) & 0xfffff;
        int64_t lower = (offset & 0xffff);
        /* use $a0($4) */
        inst1 = 0x1e000000 | (upper <<  5) | (4);      /* pcaddu18i */
        inst2 = 0x4c000000 | (lower << 10) | (4 << 5); /* jirl */
        qatomic_set(p + 0, inst1);
        qatomic_set(p + 1, inst2);
        flush_idcache_range((uintptr_t)p, (uintptr_t)p, 8);
    } else {
        assert(0);
    }
}

static void latxs_tb_set_jmp_target(TranslationBlock *tb,
        int n, TranslationBlock *next_tb)
{
#ifdef CONFIG_SOFTMMU
    bool is_cross = n ? check_misc_flags(tb, MF_NTB_CROSSPAGE_1) :
                        check_misc_flags(tb, MF_NTB_CROSSPAGE_0);
    bool is_cp_tb = (tb_page_addr1(next_tb) != -1) ? true : false;
#else
    /* no cross-page problem in user mode */
    bool is_cross = false;
    bool is_cp_tb = false;
#endif
    lsassert(next_tb != NULL);
    TBNextData *data = &tb->next_tb_data[n];

    data->pc = next_tb->pc;
    data->page_addr_ptw[0] = next_tb->page_addr[0] >> TARGET_PAGE_BITS;
    data->page_addr_ptw[1] = next_tb->page_addr[1] >> TARGET_PAGE_BITS;
    data->page_addr_cache[0] = next_tb->page_addr_mix;
    data->page_addr_cache[1] = next_tb->page_addr[1] == -1 ? 0 :
        tb_cal_page_addr_mix(next_tb->pc + TARGET_PAGE_SIZE,
                             next_tb->page_addr[1]);
#ifdef CONFIG_LATX_TU
    latxs_tu_generation_offset_link(tb, n, next_tb);
#endif

    uint64_t target_ptr = 0;

    if (option_int_opt == IRQ_CHECK_BLOCK_START && next_tb->pc > tb->pc) {
        target_ptr = (uint64_t)next_tb->tc.ptr +
                     (next_tb->label_pos_nocheck << 2);
    } else {
        target_ptr = (uint64_t)next_tb->tc.ptr;
    }

    if (target_ptr) {
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
        if (option_external_tu_stub && !in_aot_translate &&
            !(tb->bool_flags & IS_AOT_TB) &&
            tucp_fast_direct_check() && !tb_is_tu(tb) &&
            (tb->flags & HF_CPL_MASK) == HF_CPL_MASK &&
            tb_is_tu(next_tb) && TB_TU_PAGES(next_tb)) {
            void *entry = latxs_external_tu_stub(tb, n, (void *)target_ptr);
            if (entry) {
                __latxs_tb_set_jmp_target(tb, n, entry, false);
            }
            return;
        }
#endif
        bool use_cp_label = !is_cross && !is_cp_tb;
#ifdef CONFIG_LATX_TU
        /*
         * Ordinary JIT TBs also carry an id-0 pages descriptor for page
         * tracking, but only real TU targets require the direct TU page check.
         * Keep ordinary targets on the normal direct-link path.
         */
        if (tucp_fast_direct_check() && tb_is_tu(next_tb)) {
            use_cp_label = false;
        }
        if (latxs_tucp_link_can_bypass_check(tb, next_tb)) {
            use_cp_label = true;
        }
#endif
        /* The TU check label follows the normal interrupt check.  When the
         * source TU already covers every target page, patching this label
         * removes only the redundant TUCHECK, not interrupt handling. */
        __latxs_tb_set_jmp_target(tb, n, (void *)target_ptr,
                use_cp_label);
    }
}

/* check fpu rotate and patch the native jump address */
void latx_tb_set_jmp_target(TranslationBlock *tb, int n,
        TranslationBlock *next_tb)
{
#ifdef CONFIG_LATX_DEBUG
    if (tb->inhibit_irq_type == 1) {
        /* type1 = set   the inhibitirq in env.hflags
         * type2 = clear the inhibitirq in env.hflags
         * type1 TB must be linked to type2 TB to make sure the
         * inhibitirq in env.hflags will be cleared.  */
        if (next_tb->inhibit_irq_type != 2) {
            fprintf(stderr, "%s:%d \033[31m unexpected tb link. \033[m " \
                    "TB %p -> %p PC %lx -> %lx " \
                    "inhibitirq %d -> %d\n", __func__, __LINE__,
                    tb, next_tb ,tb->pc, next_tb->pc,
                    tb->inhibit_irq_type, next_tb->inhibit_irq_type);
        }
    }
#endif
    latxs_tb_set_jmp_target(tb, n, next_tb);
}

void latx_tb_reset_jmp_target(TranslationBlock *tb, int n)
{
    uint64_t inst_b2  = 0x50000800;
    uint64_t inst_nop = 0x03400000;
    uint64_t inst = (inst_nop << 32) | inst_b2;
    TBNextData *data = &tb->next_tb_data[n];

    memset(data, 0, sizeof(*data));
    data->pc = tb->pc;

    void *jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
            tb->jmp_insn_offset[n]);
    if (tb->jmp_cp_data[n] & TB_JMP_CP_DATA_LINKED) {
        int jmp_cp_off = tb->jmp_cp_data[n] & TB_JMP_CP_DATA_OFF;
        jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
                (tb->jmp_insn_offset[n] - (jmp_cp_off << 2)));
        /*
         * +-------+            +-------+  <-- jmp_inst_ptr - (jmp_cp_off << 2)
         * | inst1 |    link    | link  |
         * | inst2 |   >>>>>>   | codes |
         * |       |            |       |
         * +-------+   unlink   +-------+  <-- jmp_inst_ptr
         * | b 2   |   <<<<<<   | b 2   |
         * | nop   |            | nop   |
         * +-------+            +-------+
         */
        tb->jmp_cp_data[n] &= ~TB_JMP_CP_DATA_LINKED;
        inst = tb->jmp_inst_hold[n];
    }
    if (tb->link_opt[n] == 1) {
#ifdef CONFIG_LATX_TU
        latxs_tu_generation_offset_unlink(tb, n);
#ifdef CONFIG_SOFTMMU
        latxs_external_tu_stub_unlink(tb, n);
#endif
#endif
        return;
    }
    if (tb->link_opt[n] == 2) {
        tb->link_opt[n] = 1;
        inst = tb->jmp_inst_hold[n];
    }

    qatomic_set((uint64_t*)jmp_inst_ptr, inst);
    flush_idcache_range((uintptr_t)jmp_inst_ptr, (uintptr_t)jmp_inst_ptr, 8);
#ifdef CONFIG_LATX_TU
    latxs_tu_generation_offset_unlink(tb, n);
#ifdef CONFIG_SOFTMMU
    latxs_external_tu_stub_unlink(tb, n);
#endif
#endif
}
