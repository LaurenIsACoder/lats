#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import re
import sys


root = Path(__file__).resolve().parents[4]
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
ras_source = (root / "target/i386/latx/optimization/sys-ras.c").read_text()
save_source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
codec_source = (root / "target/i386/latx/sbt/sys-aot-tu-codec.c").read_text()
load_source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


translate_tu = source_block(tu_source, "int translate_tu(",
                            "TranslationBlock *tu_gen_code")
if not translate_tu:
    errors.append("translate_tu block is missing")
else:
    if not re.search(
        r"bool\s+link_runtime_ras\s*=\s*option_soft_ras\s*&&\s*"
        r"!in_aot_translate\s*;",
        translate_tu,
    ):
        errors.append("runtime RAS links are not disabled for AOT translation")
    if "in_fork_child" in translate_tu:
        errors.append("TU translation still depends on the save process type")

    for ir1_type in ("IR1_TYPE_CALL", "IR1_TYPE_CALLIN"):
        case_start = translate_tu.find(
            f"tb->s_data->last_ir1_type == {ir1_type}")
        if case_start < 0:
            errors.append(f"{ir1_type} RAS handling is missing")
            continue
        case_end = translate_tu.find(
            "tb->s_data->last_ir1_type == ", case_start + 1)
        if case_end < 0:
            case_end = translate_tu.find("if ((tb->tu_jmp", case_start)
        case = translate_tu[case_start:case_end]
        if "link_runtime_ras" not in case:
            errors.append(f"{ir1_type} does not use the AOT-safe RAS guard")
        if "latxs_ras_link" not in case:
            errors.append(f"{ir1_type} runtime RAS link is missing")

aot_create = source_block(tu_source,
                          "static TranslationBlock* aot_tb_create",
                          "static int get_tb_id")
if not aot_create:
    errors.append("AOT TB creation block is missing")
elif "tcg_tb_insert(" in aot_create:
    errors.append(
        "private AOT retranslation TB is published during creation")

save_tu = source_block(save_source, "static bool save_one_tu(",
                       "static bool save_tu_by_page(")
if not save_tu:
    errors.append("TU serialization block is missing")
else:
    if re.search(r"^\s*latxs_ras_unlink\s*\(", save_tu,
                 re.MULTILINE):
        errors.append(
            "AOT TU save permanently disables relinkable software-RAS sites")
    if "aot_encode_tu(" not in save_tu:
        errors.append("TU save bypasses the common code serializer")
    if "latxs_ras_close_relinkable" not in codec_source:
        errors.append(
            "AOT TU save does not close software-RAS sites relinkably")

close_relinkable = source_block(ras_source,
                                "bool latxs_ras_close_relinkable(",
                                "bool latxs_ras_link(")
if not close_relinkable:
    errors.append("relinkable software-RAS close operation is missing")
else:
    if "latxs_ras_patch_detach" not in close_relinkable:
        errors.append("relinkable close does not close the generated-code gate")
    if "latxs_ras_detach" not in close_relinkable:
        errors.append("relinkable close does not detach the reverse RAS edge")
    if "latxs_ras_set_flag(tb, MF_RAS_CLEAR_B0)" in close_relinkable:
        errors.append("relinkable close permanently invalidates the RAS site")

activate = source_block(load_source, "static TranslationBlock *activate_tb(",
                        "inline int recover_tb_head(")
if not activate or not re.search(
        r"if \(option_soft_ras && !option_rastoptp && tb->inst_ptr_offset &&",
        activate):
    errors.append("inactive r2 AOT sources can reach permanent unlink")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: AOT retranslation preserves relinkable software-RAS sites")
