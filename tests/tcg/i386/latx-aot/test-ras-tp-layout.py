#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Reject corrupt persisted RAS positions before AOT code is patched."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-format.c").read_text()
start = source.index("static bool aot_ras_tp_offsets_valid(")
body = source[start:source.index("\n}", start) + 2]
def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]

rel_body = "\n".join(function("static bool " + name + "(") for name in (
    "aot_reloc_slots_valid", "aot_ras_tp_control_overlap",
    "aot_ras_tp_generation_patch_valid", "aot_ras_tp_relocs_valid"))
code_start = source.index("static bool aot_ras_tp_code_valid(")
code_body = source[code_start:source.index("\n}", code_start) + 2]
header = (root / "include/lats/aot-format.h").read_text()
enum_start = header.index("typedef enum aot_rel_kind {")
rel_enum = header[enum_start:header.index("} aot_rel_kind;", enum_start) + len("} aot_rel_kind;")]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MF_RAS_CLEAR_B0 0x400
#define MF_RAS_LINKED 0x800
#define LATX_RAS_TP_CALL_SKIP (0x50000000U | (7U<<10))
#define AOT_RAS_TP_CODEGEN (UINT32_C(1) << 23)
#define LATX_RAS_TP_CALL_COLD_INSNS 5
typedef struct { int inst_ptr_offset; uint32_t ras_tp_cold_offset; uint32_t misc_flags; } aot_block;
typedef struct { int kind; uint32_t tc_offset, rel_slots_num;
    uint64_t extra_addend; } aot_rel;
''' + rel_enum + body + code_body + rel_body + r'''
int main(void)
{
    aot_block block = {32, 128, 0};
    uint32_t tp = AOT_RAS_TP_CODEGEN;
    assert(aot_ras_tp_offsets_valid(&block, 256, 0, 256, tp));
    block = (aot_block){96, 256, 0};
    assert(aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.inst_ptr_offset = -4;
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.inst_ptr_offset = 60; /* before this TU member */
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.inst_ptr_offset = 116; /* patch extends into another member */
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.inst_ptr_offset=72; /* prefix starts before this member */
    assert(!aot_ras_tp_offsets_valid(&block,1024,64,64,tp));
    block = (aot_block){96, 98, 0};
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.ras_tp_cold_offset = 96; /* no forward repair branch */
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.ras_tp_cold_offset = 1012; /* repair extends past the TU */
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.ras_tp_cold_offset = 1008; /* the old 16-byte footer would fit */
    assert(!aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block.ras_tp_cold_offset = 1004;
    assert(aot_ras_tp_offsets_valid(&block, 1024, 64, 64, tp));
    block = (aot_block){4, (1U << 27) + 4, 0};
    assert(!aot_ras_tp_offsets_valid(&block, 1U << 28, 0, 64, tp));
    block = (aot_block){0, 0, 0};
    assert(aot_ras_tp_offsets_valid(&block, 0, 0, 0, tp));
    aot_rel rels[3] = {{LOAD_TB_FULL_ADDR, 128, 3, 0},
                       {RAS_TP_REPAIR, 140, 1, 1}};
    block = (aot_block){32,128,0};
    assert(aot_ras_tp_relocs_valid(&block, rels, 2, 0, tp));
    assert(!aot_ras_tp_relocs_valid(&block, rels, 1, 0, tp));
    for (int offset = -4; offset <= 4; offset += 8) {
        rels[1].tc_offset = 140 + offset;
        assert(!aot_ras_tp_relocs_valid(&block, rels, 2, 0, tp));
    }
    rels[1].tc_offset = 140; rels[1].rel_slots_num = 2;
    assert(!aot_ras_tp_relocs_valid(&block, rels, 2, 0, tp));
    rels[1].rel_slots_num = 1; rels[1].extra_addend = 0;
    assert(!aot_ras_tp_relocs_valid(&block, rels, 2, 0, tp));
    rels[1].extra_addend = 1; rels[2] = rels[1];
    assert(!aot_ras_tp_relocs_valid(&block, rels, 3, 0, tp));
    rels[2] = (aot_rel){B_PROLOGUE,136,2,0};
    assert(!aot_ras_tp_relocs_valid(&block, rels, 3, 0, tp));
    /* TU persisted cold is absolute, its relocations are member-relative. */
    block = (aot_block){96,192,0};
    assert(aot_ras_tp_relocs_valid(&block, rels, 2, 64, tp));
    assert(!aot_ras_tp_relocs_valid(&block, rels, 2, 0, tp));
    uint32_t code[64]={0};
    code[(block.inst_ptr_offset-12)/4]=0x0347fc49;
    code[block.inst_ptr_offset/4]=0x50000000U|((block.ras_tp_cold_offset-block.inst_ptr_offset)/4<<10);
    assert(aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    block.misc_flags=MF_RAS_LINKED;
    assert(!aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    block.misc_flags=0;code[block.inst_ptr_offset/4]=0x14000004;
    assert(!aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    code[block.inst_ptr_offset/4]=0x50000000U|((block.ras_tp_cold_offset-block.inst_ptr_offset)/4<<10);
    block.misc_flags=MF_RAS_CLEAR_B0;
    assert(!aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    code[(block.inst_ptr_offset-12)/4]=LATX_RAS_TP_CALL_SKIP;
    assert(aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    block.misc_flags|=MF_RAS_LINKED;
    assert(!aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    block.misc_flags=0;
    assert(!aot_ras_tp_code_valid(&block,(const uint8_t *)code,tp));
    rels[2]=(aot_rel){B_PROLOGUE,20,1,0}; /* first word of a TU member */
    assert(!aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){B_PROLOGUE,32,1,0}; /* gate of a TU member */
    assert(!aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){B_PROLOGUE,28,2,0}; /* starts before, overlaps the gate */
    assert(!aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){B_PROLOGUE,28,1,0}; /* ends immediately before the gate */
    assert(aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){SC_TABLE_BEGIN,28,1,0};
    assert(!aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){SC_TABLE_BEGIN,28,2,0};
    assert(!aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    rels[2]=(aot_rel){SC_TABLE_BEGIN,24,2,0};
    assert(aot_ras_tp_relocs_valid(&block,rels,3,64,tp));
    for (int kind=LOAD_TB_ADDR;kind<=LOAD_TB_FULL_ADDR;kind++) {
        aot_rel short_rel={kind,40,2,0};
        assert(!aot_reloc_slots_valid(&short_rel));
        short_rel.rel_slots_num=3;assert(aot_reloc_slots_valid(&short_rel));
    }
    aot_rel helper={LOAD_HELPER_BEGIN+1,40,2,0};
    assert(!aot_reloc_slots_valid(&helper));
    helper.rel_slots_num=3;assert(aot_reloc_slots_valid(&helper));
    aot_rel guest={LOAD_GUEST_ADDR,40,1,0};
    assert(!aot_reloc_slots_valid(&guest));
    guest.rel_slots_num=2;assert(aot_reloc_slots_valid(&guest));
    guest.rel_slots_num=3;assert(aot_reloc_slots_valid(&guest));
    aot_rel after_njc={INTB_FSHT_AFTER_NJC,40,0,1};
    assert(!aot_reloc_slots_valid(&after_njc));
    after_njc.rel_slots_num=1;
    assert(aot_reloc_slots_valid(&after_njc));
    after_njc.rel_slots_num=2;
    assert(!aot_reloc_slots_valid(&after_njc));
    aot_rel njc_return={INTB_NJC_RETURN,40,0,1};
    assert(!aot_reloc_slots_valid(&njc_return));
    njc_return.rel_slots_num=1;assert(aot_reloc_slots_valid(&njc_return));
    njc_return.rel_slots_num=2;assert(!aot_reloc_slots_valid(&njc_return));
    assert(!aot_ras_tp_generation_patch_valid(&block,16,64,tp)); /* first-4 */
    assert(!aot_ras_tp_generation_patch_valid(&block,28,64,tp)); /* gate-4 */
    assert(!aot_ras_tp_generation_patch_valid(&block,124,64,tp)); /* cold-4 */
    assert(aot_ras_tp_generation_patch_valid(&block,12,64,tp));
    assert(aot_ras_tp_generation_patch_valid(&block,24,64,tp));
    assert(aot_ras_tp_generation_patch_valid(&block,36,64,tp));
    assert(aot_ras_tp_generation_patch_valid(&block,120,64,tp));
    assert(aot_ras_tp_generation_patch_valid(&block,28,64,0));
    block.inst_ptr_offset = 0;
    assert(!aot_ras_tp_relocs_valid(&block, rels, 2, 64, tp));
    assert(aot_ras_tp_relocs_valid(&block, NULL, 0, 64, tp));
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-layout-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"], input=program,
                   text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: AOT RAS gate/repair positions stay inside their member and TU")
