#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[4]
tb_header = (root / "include/exec/translation-block.h").read_text()
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
eob_source = (root / "target/i386/latx/translator/sys-eob.c").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


if "#define TU_INDIRECT_EXIT_INLINE_PENDING (UINT32_MAX - 1)" not in tb_header:
    errors.append("indirect inline-exit eligibility state is missing")
if "TU_COLD_EXIT_INDIRECT_DEFERRED" in tb_header:
    errors.append("obsolete indirect cold-tail state remains")

prepare = source_block(
    tu_source, "static bool tu_indirect_call_inline_exit_eligible",
    "static bool tu_direct_jump_cold_exit_eligible")
if not prepare:
    errors.append("TU-local indirect-call eligibility is missing")
else:
    for token in (
        "in_aot_translate", "option_aot_save",
        "option_int_opt != IRQ_CHECK_BLOCK_END", "i + 1 >= tb_num_in_tu",
        "IR1_TYPE_CALLIN", "CF_NO_GOTO_TB", "CF_NO_GOTO_PTR",
        "TU_TB_INDEX_NEXT", "next != tb_list[i + 1]",
        "next->pc != tb->s_data->x86_next_pc",
        "get_page(tb->pc) != get_page(tb->s_data->x86_next_pc)",
    ):
        if token not in prepare:
            errors.append(f"TU-local indirect-call eligibility misses {token}")
    hras_prepare = source_block(prepare, "#ifndef CONFIG_USER_ONLY", "#endif")
    for token in (
        "option_tu_hras && option_rastoptp && option_soft_ras",
        "HF_CS64_MASK", "!tb->cs_base", "HF_CPL_MASK", "return true;",
    ):
        if token not in (hras_prepare or ""):
            errors.append(f"HRAS indirect-call eligibility misses {token}")
    generic_prepare = prepare.replace(hras_prepare or "", "", 1)
    for token in ("option_soft_ras", "option_hardware_ras", "MF_RAS_PORTAL"):
        if token in generic_prepare:
            errors.append(f"indirect-call eligibility depends on {token}")

prepare_calls = source_block(
    tu_source, "static bool tu_direct_jump_cold_exit_eligible",
    "int translate_tu")
if not prepare_calls or not re.search(
    r"tu_indirect_call_inline_exit_eligible\(\s*"
    r"tb_num_in_tu, tb_list, i\).*?TU_INDIRECT_EXIT_INLINE_PENDING",
    prepare_calls, re.S):
    errors.append("TU preparation does not classify eligible indirect CALLs")

inline_eligible = source_block(
    eob_source, "static bool latxs_tr_exit_tb_use_tu_indirect_inline_exit",
    "static void latxs_tr_exit_tb_emit_indirect_context_switch")
if not inline_eligible:
    errors.append("final indirect inline-exit transition is missing")
else:
    for token in (
        "tb_is_tu(tb)", "TU_INDIRECT_EXIT_INLINE_PENDING",
        "option_int_opt == IRQ_CHECK_BLOCK_END", "ctx->tr_ctx->opt_can_link",
        "ctx->tr_ctx->opt_can_indir_link", "ir1_is_indirect_call(branch)",
        "tb->s_data->tu_cold_exit_offset[succ_id] = TU_COLD_EXIT_INVALID",
        "return true",
    ):
        if token not in inline_eligible:
            errors.append(f"final indirect inline transition misses {token}")

context_switch = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_indirect_context_switch",
    "static void latxs_tr_exit_tb_emit_indirect_interrupt")
if (not context_switch or
        "latxs_tr_gen_exit_tb_j_context_switch(NULL, 0, succ_id)" not in
        context_switch):
    errors.append("indirect context-switch helper does not jump directly")

interrupt = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_indirect_interrupt",
    "static void latxs_tr_exit_tb_emit_indirect_lookup")
if not interrupt:
    errors.append("indirect interrupt emitter is missing")
else:
    for token in (
        "bool inline_tu_exit", "LISA_BGE", "lookup_label",
        "latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id)",
    ):
        if token not in interrupt:
            errors.append(f"inline indirect interrupt exit misses {token}")
    inline_branch = source_block(interrupt, "if (inline_tu_exit)", "} else")
    if inline_branch and "latxs_ir2_interrupt_get_label" in inline_branch:
        errors.append("inline indirect interrupt still jumps through a TU-tail label")

indirect_action = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_indirect_action",
    "static void latxs_tr_exit_tb_prepare")
if not indirect_action or not re.search(
    r"inline_tu_exit\s*=\s*"
    r"latxs_tr_exit_tb_use_tu_indirect_inline_exit\(\s*"
    r"ctx, branch, succ_id\);.*?"
    r"emit_indirect_interrupt\(\s*ctx, succ_id, inline_tu_exit,\s*"
    r"inline_hras_call \? &njc_miss : NULL,\s*"
    r"inline_tu_exit \? hras_fsht_id : -1\);.*?"
    r"if \(inline_hras_call\).*?return;\s*\}\s*"
    r"latxs_tr_exit_tb_emit_indirect_lookup\(ctx, hras_call\);.*?"
    r"if \(!inline_tu_exit\).*?"
    r"latxs_tr_exit_tb_emit_indirect_slowpath\(ctx, succ_id\);",
    indirect_action or "", re.S):
    errors.append("eligible inline exit does not directly precede lookup")

if not re.search(
    r"bool hras_call = false;.*?bool inline_hras_call = false;.*?"
    r"hras_call = option_tu_hras && option_rastoptp && option_soft_ras.*?"
    r"ir1_is_indirect_call\(branch\).*?ctx->tr_ctx->ras_tp_has_call;.*?"
    r"inline_hras_call = hras_call && option_intb_njc;",
    indirect_action or "", re.S):
    errors.append("dedicated CALL lookup is not gated by HRAS and a software push")

cold_tail = source_block(eob_source, "void latxs_tr_gen_tu_cold_exits",
                         "void latxs_tr_generate_exit_tb")
if not cold_tail:
    errors.append("TU cold-tail generator is missing")
else:
    for token in ("TU_COLD_EXIT_INDIRECT_DEFERRED",
                  "latxs_tr_exit_tb_emit_indirect_slowpath"):
        if token in cold_tail:
            errors.append(f"TU tail still contains indirect one-hop exit: {token}")

generate_exit = eob_source[eob_source.find("void latxs_tr_generate_exit_tb"):]
if "TU_INDIRECT_EXIT_INLINE_PENDING" not in generate_exit:
    errors.append("final cleanup does not invalidate unconsumed indirect state")
if "TU_COLD_EXIT_INDIRECT_DEFERRED" in generate_exit:
    errors.append("final cleanup retains obsolete indirect cold-tail state")

fix_offsets = source_block(
    tu_source, "static void fix_tu_cold_exit_offsets",
    "static void gpr_highbits_reset")
if not fix_offsets or "TU_INDIRECT_EXIT_INLINE_PENDING" not in fix_offsets:
    errors.append("label disposal does not invalidate unconsumed indirect state")
if fix_offsets and "TU_COLD_EXIT_INDIRECT_DEFERRED" in fix_offsets:
    errors.append("label disposal retains obsolete indirect cold-tail state")

if re.search(r"LISA_BL|lisa_call|LISA_JIRL[^\n]*latxs_ra",
             indirect_action or ""):
    errors.append("indirect CALL layout adds a call/ra transfer")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)
print("PASS: TU indirect CALL/JMP inline interrupt-exit layout policy")
