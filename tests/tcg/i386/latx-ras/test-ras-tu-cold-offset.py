#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Keep a published RAS repair independent of asynchronous TU exit labels."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[4]
def function(path,name):
 s=(root/path).read_text();a=s.index(name);b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
h=(root/'include/exec/translation-block.h').read_text()
a=h.index('struct separated_data{');b=h.index('\n};',a)+3
ras=(root/'target/i386/latx/include/latx-ras.h').read_text()
macro=re.search(r'#ifdef CONFIG_LATX_TU\n#define LATXS_RAS_TP_COLD_OFFSET.*?#endif',ras,re.S).group()
program='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_LATX_TU 1
#define g_assert assert
#define TU_COLD_EXIT_INVALID 0
#define TU_COLD_EXIT_DIRECT_PENDING UINT32_MAX
#define TU_INDIRECT_EXIT_INLINE_PENDING (UINT32_MAX-1)
#define TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED (UINT32_MAX-3)
typedef struct TranslationBlock TranslationBlock;
typedef struct TranslationUnitPages TranslationUnitPages;
'''+h[a:b]+''';
struct TranslationBlock { struct separated_data *s_data; int inst_ptr_offset; };
static int option_rastoptp,option_sys_aot,option_pushpop,option_split_search=1;
static bool tucp_fast_needs_pages_desc(void) { return false; }
'''+macro+'\n'
program+=function('tcg/tcg.c','static bool tcg_tb_needs_private_s_data(')
program+=function('target/i386/latx/optimization/sys-tu.c','static void fix_tu_cold_exit_offsets(')
program+=function('target/i386/latx/optimization/sys-ras.c','uint32_t latxs_ras_tp_closed_gate(')
program+='''int main(void) {
 _Static_assert(sizeof(struct separated_data)<=128,"sidecar grew");
 assert(!tcg_tb_needs_private_s_data(false));
 option_rastoptp=1;
 assert(tcg_tb_needs_private_s_data(false));
 struct separated_data sd={0}; TranslationBlock tb={&sd,16};
 TranslationBlock *list[]={&tb}; int labels[]={0,10,20};
 LATXS_RAS_TP_COLD_OFFSET(&tb)=128;
 uint32_t branch=latxs_ras_tp_closed_gate(&tb);
 sd.tu_cold_exit_offset[0]=1;sd.tu_cold_exit_offset[1]=2;
 fix_tu_cold_exit_offsets(list,1,labels);
 assert(sd.tu_cold_exit_offset[0]==40 && sd.tu_cold_exit_offset[1]==80);
 assert(LATXS_RAS_TP_COLD_OFFSET(&tb)==128);
 sd.tu_cold_exit_offset[0]=sd.tu_cold_exit_offset[1]=0;
 assert(latxs_ras_tp_closed_gate(&tb)==branch);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(program)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: private RAS repair metadata survives TU exit reset and resolution; sidecar <=128')
