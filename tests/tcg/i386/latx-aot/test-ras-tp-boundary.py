#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Interrupt every emitted instruction of the real r2/C boundary sequences."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-ras.c").read_text()


def function(signature):
    begin = source.index(signature)
    return source[begin:source.index("\n}", begin) + 2]


prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define LATX_RAS_TP_SIZE 512
typedef struct { uint32_t reset_pending, top; uint64_t native_tp; } LatxRasTPState;
typedef struct { LatxRasTPState ras_tp; } CPUX86State;
#define RAS_TP_OFF(m) offsetof(CPUX86State,ras_tp.m)
#define qatomic_set(p,v) (*(p)=(v))
#define qatomic_read(p) (*(p))
enum { IR2_OPND_GPR, IR2_OPND_LABEL };
typedef struct { int type, value; } IR2_OPND;
enum { LISA_ADDI_D, LISA_STPTR_D, LISA_LDPTR_D, LISA_BEQ, LISA_STPTR_W,
       LISA_ADD_D, LISA_AMSWAP_W, LISA_BEQZ, LISA_LDPTR_W, LABEL, IMM };
typedef struct { int op; IR2_OPND rd,rj,rk; int64_t imm; } Instruction;
static Instruction code[128]; static int count, label_id;
static int option_rastoptp=1;
static IR2_OPND la_sp={0,3},la_env={0,23},la_zero={0,0};
static IR2_OPND latxs_ir2_opnd_new(int type,int reg) { return (IR2_OPND){type,reg}; }
static IR2_OPND latxs_ir2_opnd_new_label(void) { return (IR2_OPND){1,++label_id}; }
static int hamt_enabled(void) { return 0; }
static void latxs_tr_dmw_in_fix_mem(const IR2_OPND *reg) { (void)reg; }
static void latxs_tr_dmw_out_fix_mem(const IR2_OPND *reg) { (void)reg; }
static void la_out2i(int op,const IR2_OPND *rd,const IR2_OPND *rj,int64_t imm) {
    code[count++]=(Instruction){op,*rd,*rj,{0,0},imm};
}
static void la_out3(int op,const IR2_OPND *rd,const IR2_OPND *rj,const IR2_OPND *rk) {
    code[count++]=(Instruction){op,*rd,*rj,*rk,0};
}
static void la_out2(int op,const IR2_OPND *rd,const IR2_OPND *rj) {
    code[count++]=(Instruction){op,*rd,*rj,{0,0},0};
}
static void la_label(IR2_OPND label) {
    code[count++]=(Instruction){LABEL,label,{0,0},{0,0},0};
}
static void latxs_load_imm64_to_ir2(const IR2_OPND *rd,uint64_t value) {
    code[count++]=(Instruction){IMM,*rd,{0,0},{0,0},value};
}
'''
checks = r'''
static int branch(int label) {
    for(int i=0;i<count;i++)if(code[i].op==LABEL && code[i].rd.value==label)return i;
    assert(0);return 0;
}
static uint64_t load(uint64_t addr,int size) {
    uint64_t value=0;memcpy(&value,(void *)(uintptr_t)addr,size);return value;
}
static void store(uint64_t addr,uint64_t value,int size) {
    memcpy((void *)(uintptr_t)addr,&value,size);
}
static void run(int leave,int pending,int interrupt_at) {
    CPUX86State env={.ras_tp={pending,32,UINT64_C(0x123456789abc)}};
    uint64_t stack[256]={0},r[32]={0};
    r[2]=leave?env.ras_tp.native_tp:32;
    r[3]=(uintptr_t)&stack[200]; r[23]=(uintptr_t)&env;
    r[20]=0xaaaa; r[21]=0xbbbb;
    uint64_t initial_sp=r[3]; int steps=0;
    for(int pc=0;pc<count;pc++,steps++) {
        if(steps==interrupt_at)latxs_ras_tp_save(&env,r);
        Instruction *i=&code[pc]; int rd=i->rd.value,rj=i->rj.value,rk=i->rk.value;
        uint64_t addr=r[rj]+i->imm*4;
        switch(i->op) {
        case LISA_ADDI_D:r[rd]=r[rj]+i->imm;break;
        case LISA_ADD_D:r[rd]=r[rj]+r[rk];break;
        case LISA_STPTR_D:store(addr,r[rd],8);break;
        case LISA_STPTR_W:store(addr,r[rd],4);break;
        case LISA_LDPTR_D:r[rd]=load(addr,8);break;
        case LISA_LDPTR_W:r[rd]=(int32_t)load(addr,4);break;
        case LISA_AMSWAP_W: {
            uint32_t old=load(r[rj],4);store(r[rj],r[rk],4);r[rd]=(int32_t)old;break;
        }
        case LISA_BEQ:if(r[rd]==r[rj])pc=branch(rk);break;
        case LISA_BEQZ:if(!r[rd])pc=branch(rj);break;
        case LABEL:break;
        case IMM:r[rd]=i->imm;break;
        default:assert(0);
        }
        r[0]=0;
    }
    assert(env.ras_tp.top==(pending?0:32));
    assert(!env.ras_tp.reset_pending);
    assert(r[2]==(leave?(pending?0:32):env.ras_tp.native_tp));
    assert(r[3]==initial_sp && r[20]==0xaaaa && r[21]==0xbbbb);
}
int main(void) {
    for(int leave=0;leave<2;leave++) {
        count=label_id=0;
        if(leave)latxs_ras_tp_leave_c();else latxs_ras_tp_enter_c();
        for(int pending=0;pending<2;pending++)
            for(int cut=-1;cut<count+1;cut++)run(leave,pending,cut);
    }
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-boundary-") as directory:
    binary = Path(directory) / "test"
    program = prefix + function("void latxs_ras_tp_enter_c(") + function(
        "void latxs_ras_tp_leave_c(") + function("void latxs_ras_tp_save(") + checks
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: C-boundary interrupts preserve top and never revive a consumed reset")
