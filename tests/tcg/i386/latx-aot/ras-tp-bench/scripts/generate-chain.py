#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Emit the 600 distinct direct CALL sites used by the nested-return test."""
from pathlib import Path
import argparse
import shutil
p=argparse.ArgumentParser()
p.add_argument("out",type=Path)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
direct=Path(__file__).resolve().parent.parent/"direct"
source=(direct/"kernels.c").read_text().split("const struct kernel kernels[]")[0].replace("#include <stdlib.h>","#include <stdlib.h>\n#include <unistd.h>")
for level in reversed(range(600)):
    target="callee0" if level==599 else "level_"+str(level+1)
    source+=f"NOINLINE static uint64_t level_{level}(uint64_t x) {{uint64_t y={target}(x+{level+1});return (y^x)+UINT64_C(0x123456789abcdef);}}\n"
for depth in (4,32,128,500,511,512,513,600):
    source+=f"NOINLINE static uint64_t chain{depth}(uint64_t n,uint64_t s) {{for(uint64_t i=0;i<n;i++)s=level_{600-depth}(s);return s;}}\n"
source+='static volatile int pid_sink;\nNOINLINE static uint64_t syscall_leaf(uint64_t x) {pid_sink=getpid();if(pid_sink<=0)abort();return callee0(x);}\n'
for level in reversed(range(32)):
    target="syscall_leaf" if level==31 else "syscall_level_"+str(level+1)
    source+=f"NOINLINE static uint64_t syscall_level_{level}(uint64_t x) {{uint64_t y={target}(x+{level+1});return (y^x)+UINT64_C(0x123456789abcdef);}}\n"
source+='NOINLINE static uint64_t chain_syscall(uint64_t n,uint64_t s) {for(uint64_t i=0;i<n;i++)s=syscall_level_0(s);return s;}\nconst struct kernel kernels[]={\n    {"loop_dep",loop_dep},{"loop_ilp",loop_ilp},{"branch_pred",branch_pred},{"branch_random",branch_random},\n    {"call_direct",call_direct},{"call_indirect1",call_indirect1},{"call_indirect4",call_indirect4},\n    {"chase_l1",chase_l1},{"chase_large",chase_large},{"chain4",chain4},{"chain32",chain32},{"chain128",chain128},{"chain500",chain500},{"chain511",chain511},{"chain512",chain512},{"chain513",chain513},{"chain600",chain600},{"chain_syscall",chain_syscall}\n};\nconst size_t kernel_count=sizeof(kernels)/sizeof(kernels[0]);\n'
(a.out/"kernels.c").write_text(source)
for name in ("main.c","kernels.h","callees.c"):
    origin=direct if name=="callees.c" else direct.parent/"common"
    shutil.copy2(origin/name,a.out/name)
