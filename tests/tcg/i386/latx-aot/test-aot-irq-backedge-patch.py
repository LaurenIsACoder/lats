#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check actual TU B relocation preserves the in-TB IRQ cold instructions."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[4]
source=(root/'target/i386/latx/optimization/sys-tu.c').read_text()
header=(root/'target/i386/latx/include/latx-tu.h').read_text()
start=source.index('int tu_relocat_target_branch(');end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
f=source[start:end]
names=['BCC_OPCODE','BEQ_OPCODE','BNE_OPCODE','BLT_OPCODE','BGE_OPCODE','BLTU_OPCODE','BGEU_OPCODE','BEQZ_OPCODE','BNEZ_OPCODE','BCEQZ_OPCODE','BCNEZ_OPCODE','B_OPCODE','OFF26_MIN','OFF26_MAX','OFF16_MIN','OFF16_MAX','OFF20_MIN','OFF20_MAX']
defines='\n'.join(re.search(r'^#define\s+'+name+r'\s+[^\n]+',header,re.M).group() for name in names)
c=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#define TU_TB_INDEX_TARGET 1
#define TU_TB_INDEX_NEXT 0
''' + defines + r'''
typedef struct TB TB;
typedef struct {TB *next_tb[2];uint32_t tu_jmp[2];} Side;
struct TB {Side *s_data;struct {void *ptr;unsigned offset_in_tu;} tc;};
typedef TB TranslationBlock;
static uint32_t bcc_ins_convert(uint32_t inst){(void)inst;abort();}
''' + f + r'''
int main(void){
    static uint32_t words[200000];const int src=100000;
    const int distances[]={-32768,-4096,-1,0,1,4096,32767};unsigned cases=0;
    for(unsigned member_off=0;member_off<3;member_off++)for(unsigned tu_off=0;tu_off<3;tu_off++)for(unsigned i=0;i<sizeof(distances)/sizeof(distances[0]);i++){
        int off=distances[i];words[src]=B_OPCODE;words[src+1]=0x12345678;words[src+2]=0x87654321;
        TB target={.tc.ptr=&words[src+off]};Side side={0};TB tb={.s_data=&side,.tc.ptr=&words[src-member_off+tu_off],.tc.offset_in_tu=tu_off*4};
        side.next_tb[TU_TB_INDEX_TARGET]=&target;side.tu_jmp[TU_TB_INDEX_TARGET]=member_off*4;
        assert(!tu_relocat_target_branch(&tb));
        uint32_t expected=B_OPCODE|(((uint32_t)(off>>16))&0x3ff)|(((uint32_t)off&0xffff)<<10);
        assert(words[src]==expected&&words[src+1]==0x12345678&&words[src+2]==0x87654321);cases++;
    }
    printf("PASS: %u actual TU jump patch cases preserve following IRQ cold instructions\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='aot-irq-patch-') as d:
    p=Path(d);(p/'test.c').write_text(c)
    subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
