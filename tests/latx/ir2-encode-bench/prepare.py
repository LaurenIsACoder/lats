#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Extract the actual encoders and their tables for a standalone LSX test."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("baseline", type=Path, help="unmodified ir2-assemble.c")
p.add_argument("output", type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
inc = root / "target/i386/latx/include"
candidate = root / "target/i386/latx/ir2/ir2-assemble.c"
a.output.mkdir(parents=True, exist_ok=True)

def extract(text, first):
    start = text.index(first)
    end = text.index("\nstatic\nvoid latxs_ir2_op_check_imm", start)
    return text[start:end]

old = extract(a.baseline.read_text(), "static int insn_encode_map[")
new = extract(candidate.read_text(), "typedef struct Ir2EncodeEntry")
scalar = """    int *res_t = (void *)&res;
    return ins | res_t[0] | res_t[1] | res_t[2] | res_t[3];"""
vector = """    res = __lsx_vor_v(res, __lsx_vbsrl_v(res, 8));
    res = __lsx_vor_v(res, __lsx_vbsrl_v(res, 4));
    return ins | (uint32_t)__lsx_vpickve2gr_wu(res, 0);"""
assert old.count(scalar) == 1
# Move only the reduction; keep the original table and all other instructions.
reduce_only = old.replace(scalar, vector)
vector_lines = """    res = __lsx_vor_v(res, __lsx_vbsrl_v(res, 8));
    res = __lsx_vor_v(res, __lsx_vbsrl_v(res, 4));
"""
assert new.count(vector_lines) == 1
table_only = new.replace(vector_lines, "").replace(
    "    return ins | (uint32_t)__lsx_vpickve2gr_wu(res, 0);", scalar)
prefix = '#include <lsxintrin.h>\n#include "encoder-types.h"\n'
for name, body in [("base", old), ("reduce", reduce_only),
                   ("table", table_only), ("both", new)]:
    body = body.replace("uint32 ir2_assemble_new(", f"uint32_t encode_{name}(")
    (a.output / f"encode-{name}.c").write_text(prefix + body)

header = (inc / "ir2-la-assemble.h").read_text()
types = header[header.index("typedef enum {"):header.index("GM_OPERAND_PLACE_RELATION bit_field_table[]")]
inst = (inc / "ir2-inst.h").read_text()
inst = inst[inst.index("typedef struct IR2_INST {"):inst.index("} IR2_INST;") + len("} IR2_INST;")]
(a.output / "encoder-types.h").write_text('''#ifndef IR2_ENCODER_BENCH_TYPES_H
#define IR2_ENCODER_BENCH_TYPES_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_LATX_TU 1
#define CONFIG_LATX_IR2_COMMENT 1
#define unlikely(x) __builtin_expect(!!(x), 0)
typedef uintptr_t ADDR;
#include "la-ir2.h"
''' + inst + '''
static inline IR2_OPCODE ir2_opcode(IR2_INST *p) { return (IR2_OPCODE)p->_opcode; }
''' + types + '''
extern GM_LA_OPCODE_FORMAT lisa_format_table[];
extern GM_OPERAND_PLACE_RELATION bit_field_table_new[];
uint32_t encode_base(IR2_INST *);
uint32_t encode_reduce(IR2_INST *);
uint32_t encode_table(IR2_INST *);
uint32_t encode_both(IR2_INST *);
#endif
''')
for name in ["la-ir2.h", "ir2-opcode.h", "ir2-format.h", "ir2-la-assemble.h"]:
    shutil.copyfile(inc / name, a.output / name)
(a.output / "tables.c").write_text('''#include <stdint.h>
#define CONFIG_LATX_TU 1
#include "ir2-la-assemble.h"
''')
shutil.copyfile(Path(__file__).with_name("bench.c"), a.output / "bench.c")
sources = [a.baseline, candidate, inc / "ir2-inst.h"] + [inc / n for n in
    ["la-ir2.h", "ir2-opcode.h", "ir2-format.h", "ir2-la-assemble.h"]]
(a.output / "manifest.json").write_text(json.dumps({str(f):
    hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}, indent=2) + "\n")
print(a.output)
