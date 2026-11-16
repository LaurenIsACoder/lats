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
jump_source = (root / "target/i386/latx/translator/sys-tr-misc.c").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


for token in (
    "#define TU_COLD_EXIT_INVALID 0",
    "#define TU_COLD_EXIT_DIRECT_PENDING UINT32_MAX",
    "#define TU_INDIRECT_EXIT_INLINE_PENDING (UINT32_MAX - 1)",
    "#define TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED (UINT32_MAX - 3)",
    "uint32_t tu_cold_exit_offset[2];",
):
    if token not in tb_header:
        errors.append(f"unified TU cold-exit metadata misses {token}")

prepare = source_block(
    tu_source, "static bool tu_call_cold_exit_eligible",
    "int translate_tu")
if not prepare:
    errors.append("unified TU cold-exit preparation is missing")
else:
    for token in (
        "in_aot_translate", "option_aot_save",
        "CF_NO_GOTO_TB", "CF_NO_GOTO_PTR", "IR1_TYPE_BRANCH",
        "IR1_TYPE_JUMP", "IR1_TYPE_CALL", "IR1_TYPE_JUMPIN",
        "IR1_TYPE_CALLIN", "TU_TB_INDEX_NEXT", "TU_TB_INDEX_TARGET",
        "TU_COLD_EXIT_DIRECT_PENDING", "TU_INDIRECT_EXIT_INLINE_PENDING",
    ):
        if token not in prepare:
            errors.append(f"unified TU cold-exit preparation misses {token}")
    for token in ("option_soft_ras", "option_hardware_ras", "MF_RAS_PORTAL"):
        if token in prepare:
            errors.append(
                f"unified TU cold-exit preparation depends on {token}")

if not re.search(
    r"retry:\s*tu_prepare_cold_exits\(tb_num_in_tu, tb_list\);\s*"
    r"translate_result = translate_tb_in_tu", tu_source):
    errors.append("unified TU cold-exit preparation is not refreshed on retry")

direct_eligible = source_block(
    eob_source, "static bool latxs_tr_exit_tb_tu_cold_exit_eligible",
    "static void latxs_tr_gen_exit_tb_j_tb_link_cold")
if not direct_eligible:
    errors.append("generic direct TU cold-exit eligibility is missing")
else:
    for token in (
        "TU_COLD_EXIT_DIRECT_PENDING", "TBLINK_STUB_DIRECT",
        "ctx->allow_tb_link", "ctx->target_is_indirect",
        "ctx->target_crosses_page", "ctx->target_maybe_cross_page_block",
        "succ_id",
    ):
        if token not in direct_eligible:
            errors.append(f"generic direct eligibility misses {token}")
    if "ir1_is_call(branch)" in direct_eligible:
        errors.append("generic direct eligibility remains CALL-only")

emit_link = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_link_stub",
    "static void latxs_tr_exit_tb_save_direct_next_pc")
if not emit_link:
    errors.append("direct link-stub emitter is missing")
else:
    if "state->deferred_tu_cold_exit = true" not in emit_link:
        errors.append("direct cold exit is not deferred generically")
    for token in ("latxs_ir2_interrupt_set_label", "LISA_B"):
        if token not in emit_link:
            errors.append(f"direct interrupt trampoline misses {token}")

prepare_exit = source_block(
    eob_source, "static void latxs_tr_exit_tb_prepare",
    "static void latxs_tr_exit_tb_finish")
if not prepare_exit or not re.search(
    r"state->emitted_interrupt_check.*?"
    r"!state->deferred_tu_cold_exit.*?latxs_ir2_interrupt_set_label",
    prepare_exit, re.S):
    errors.append("deferred direct interrupt labels are still emitted inline")

indirect_inline = source_block(
    eob_source, "static bool latxs_tr_exit_tb_use_tu_indirect_inline_exit",
    "static void latxs_tr_exit_tb_emit_indirect_context_switch")
if not indirect_inline:
    errors.append("generic indirect TU inline-exit transition is missing")
else:
    for token in (
        "TU_INDIRECT_EXIT_INLINE_PENDING", "TU_COLD_EXIT_INVALID",
        "ir1_is_indirect_call(branch)", "ir1_is_indirect_jmp(branch)",
        "ctx->tr_ctx->opt_can_link", "ctx->tr_ctx->opt_can_indir_link",
    ):
        if token not in indirect_inline:
            errors.append(f"generic indirect inline transition misses {token}")
    for token in ("X86_INS_RET", "ir1_is_return"):
        if token in indirect_inline:
            errors.append("RET entered the unified indirect inline-exit path")

indirect_context_switch = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_indirect_context_switch",
    "static void latxs_tr_exit_tb_emit_indirect_interrupt")
if (not indirect_context_switch or
        "latxs_tr_gen_exit_tb_j_context_switch(NULL, 0, succ_id)" not in
        indirect_context_switch):
    errors.append("generic indirect context-switch helper is missing")

indirect_interrupt = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_indirect_interrupt",
    "static void latxs_tr_exit_tb_emit_indirect_lookup")
if not indirect_interrupt:
    errors.append("generic indirect interrupt emitter is missing")
else:
    for token in (
        "inline_tu_exit", "LISA_BGE", "lookup_label",
        "latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id)",
    ):
        if token not in indirect_interrupt:
            errors.append(f"generic inline indirect exit misses {token}")

tu_jmp = source_block(jump_source, "static bool latxs_tu_translate_jmp",
                      "bool latxs_translate_jmp")
if not tu_jmp:
    errors.append("TU direct-JMP translator is missing")
else:
    for token in (
        "TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED", "LISA_BGE", "LISA_B",
        "latxs_ir2_interrupt_get_label", "target_tb->pc <= tb->pc",
    ):
        if token not in tu_jmp:
            errors.append(f"backward TU JMP cold relocation misses {token}")
    deferred = tu_jmp.find("TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED")
    inline_fallback = tu_jmp.find("cs_native_to_bt_ret_0")
    if min(deferred, inline_fallback) < 0 or deferred > inline_fallback:
        errors.append(
            "backward TU JMP does not select cold tail before fallback")

cold_tail = source_block(eob_source, "void latxs_tr_gen_tu_cold_exits",
                         "void latxs_tr_generate_exit_tb")
if not cold_tail:
    errors.append("unified TU cold-tail generator is missing")
else:
    for token in (
        "memcpy(cold_order, tb_list",
        "cold_order[j - 1]->pc > key->pc",
        "TranslationBlock *tb = cold_order[i]",
        "for (succ_id = 0; succ_id < 2; succ_id++)",
        "TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED",
        "latxs_tr_gen_exit_tb_j_context_switch",
    ):
        if token not in cold_tail:
            errors.append(f"unified TU cold tail misses {token}")
    if "qsort(tb_list" in cold_tail:
        errors.append("cold-exit ordering modifies the hot TB list")
    for token in ("TU_COLD_EXIT_INDIRECT_DEFERRED",
                  "latxs_tr_exit_tb_emit_indirect_slowpath"):
        if token in cold_tail:
            errors.append(f"TU tail retains indirect one-hop exit: {token}")

cold_emit = tu_source.find("latxs_tr_gen_tu_cold_exits(tb_num_in_tu, tb_list);")
tb_end = tu_source.find("latxs_tr_gen_tb_end();", cold_emit)
if cold_emit < 0 or tb_end < cold_emit:
    errors.append(
        "unified TU cold exits are not after all members and before epilogue")

fix_offsets = source_block(
    tu_source, "static void fix_tu_cold_exit_offsets",
    "static void latxs_label_dispose_in_tu")
if not fix_offsets:
    errors.append("unified TU cold-exit offset resolver is missing")
else:
    for token in (
        "for (succ_id = 0; succ_id < 2; succ_id++)",
        "label_pos[label_id] << 2", "TU_COLD_EXIT_DIRECT_PENDING",
        "TU_INDIRECT_EXIT_INLINE_PENDING",
        "TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED",
    ):
        if token not in fix_offsets:
            errors.append(f"unified TU offset resolver misses {token}")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: TU unified jump-related cold-tail layout policy")
