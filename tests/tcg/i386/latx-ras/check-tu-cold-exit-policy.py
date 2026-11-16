#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import re
import sys


root = Path(__file__).resolve().parents[4]
tb_header = (root / "include/exec/translation-block.h").read_text()
translate_all = (root / "accel/tcg/translate-all.c").read_text()
translate_header = (root / "target/i386/latx/include/translate.h").read_text()
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
eob_source = (root / "target/i386/latx/translator/sys-eob.c").read_text()
aot_save = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
aot_load = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
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
    "uint32_t tu_cold_exit_offset[2];",
):
    if token not in tb_header:
        errors.append(f"TU cold-exit metadata misses {token}")

for source, name in ((translate_all, "normal TB reset"),
                     (tu_source, "TU reset")):
    for token in (
        "tu_cold_exit_offset[0] = TU_COLD_EXIT_INVALID",
        "tu_cold_exit_offset[1] = TU_COLD_EXIT_INVALID",
    ):
        if token not in source:
            errors.append(f"{name} misses {token}")

if "void latxs_tr_gen_tu_cold_exits" not in translate_header:
    errors.append("unified TU cold-tail declaration is missing")

prepare = source_block(tu_source, "static bool tu_call_cold_exit_eligible",
                       "static bool tu_indirect_call_inline_exit_eligible")
if not prepare:
    errors.append("direct-CALL TU cold-exit preparation is missing")
else:
    for token in (
        "in_aot_translate", "option_aot_save",
        "i + 1 >= tb_num_in_tu", "IR1_TYPE_CALL", "CF_NO_GOTO_TB",
        "TU_TB_INDEX_NEXT", "next != tb_list[i + 1]",
        "next->pc != tb->s_data->x86_next_pc",
        "source_page != get_page(tb->s_data->x86_next_pc)",
        "source_page != get_page(target)",
        "get_page(target) != get_page(target + 15)",
    ):
        if token not in prepare:
            errors.append(f"direct-CALL preparation misses {token}")
    for token in ("option_soft_ras", "option_hardware_ras", "MF_RAS_PORTAL"):
        if token in prepare:
            errors.append(f"direct-CALL preparation depends on {token}")

prepare_all = source_block(tu_source, "static void tu_prepare_cold_exits",
                           "int translate_tu")
if not prepare_all or not re.search(
    r"tu_call_cold_exit_eligible\(tb_num_in_tu, tb_list, i\).*?"
    r"tu_cold_exit_offset\[0\] = TU_COLD_EXIT_DIRECT_PENDING",
    prepare_all, re.S):
    errors.append("unified preparation does not retain direct-CALL selection")

eligible = source_block(
    eob_source, "static bool latxs_tr_exit_tb_tu_cold_exit_eligible",
    "static void latxs_tr_gen_exit_tb_j_tb_link_cold")
if not eligible:
    errors.append("generic direct cold-exit eligibility is missing")
else:
    for token in (
        "tb_is_tu(tb)", "TU_COLD_EXIT_DIRECT_PENDING", "TBLINK_STUB_DIRECT",
        "ctx->allow_tb_link", "ctx->target_is_indirect",
        "ctx->target_crosses_page", "ctx->target_maybe_cross_page_block",
    ):
        if token not in eligible:
            errors.append(f"generic direct eligibility misses {token}")

cold_link = source_block(
    eob_source, "static void latxs_tr_gen_exit_tb_j_tb_link_cold",
    "#endif")
if not cold_link:
    errors.append("two-slot TU cold link helper is missing")
else:
    for token in (
        "latxs_tb_link_align_inside_cacheline", "jmp_reset_offset[succ_id]",
        "link_opt[succ_id] = 1", "LISA_B", "lisa_nop",
    ):
        if token not in cold_link:
            errors.append(f"two-slot TU cold link helper misses {token}")
    for token in ("LISA_JIRL", "latxs_ra_alloc_itemp"):
        if token in cold_link:
            errors.append(f"two-slot TU cold link helper contains {token}")

emit_link = source_block(eob_source,
                         "static void latxs_tr_exit_tb_emit_link_stub",
                         "static void latxs_tr_exit_tb_save_direct_next_pc")
if not emit_link or "state->deferred_tu_cold_exit = true" not in emit_link:
    errors.append("direct cold fallback is not deferred")

direct_action = source_block(
    eob_source, "static void latxs_tr_exit_tb_emit_direct_action",
    "typedef enum ExitTBContextSwitchAction")
if not direct_action or not re.search(
    r"if \(state->deferred_tu_cold_exit\).*?"
    r"if \(option_lazypc\).*?latxs_tr_exit_tb_save_direct_next_pc.*?return;",
    direct_action, re.S):
    errors.append("deferred direct action does not preserve lazypc")

cold_tail = source_block(eob_source, "void latxs_tr_gen_tu_cold_exits",
                         "void latxs_tr_generate_exit_tb")
if not cold_tail:
    errors.append("unified TU cold-tail generator is missing")
else:
    for token in (
        "IR2_OPND_LABEL", "tr_gen_branch_save_next_eip(branch, succ_id)",
        "latxs_tr_gen_exit_tb_load_tb_addr",
        "latxs_tr_gen_exit_tb_j_context_switch(&tbptr, 1, succ_id)",
    ):
        if token not in cold_tail:
            errors.append(f"unified TU cold-tail generator misses {token}")

cold_call = re.search(
    r"latxs_tr_gen_tu_cold_exits\(tb_num_in_tu, tb_list, indirect_calls,\s*"
    r"direct_calls \? direct_calls->pdata : NULL\);", tu_source)
cold_emit = cold_call.start() if cold_call else -1
tb_end = tu_source.find("latxs_tr_gen_tb_end();", cold_emit)
if cold_emit < 0 or tb_end < cold_emit:
    errors.append("TU cold tail is not emitted before the shared epilogue")

if "tu_cold_exit_offset" in aot_save:
    errors.append("TU cold-exit metadata entered the AOT save format")
for start, end, name in (
    ("static inline int recover_tb(", "uint64_t find_page", "recover_tb"),
    ("inline int recover_tb_head(", "#ifdef CONFIG_LATX_PRINT",
     "recover_tb_head"),
):
    recover = source_block(aot_load, start, end)
    if not recover:
        errors.append(f"AOT recovery function {name} is missing")
        continue
    for succ_id in range(2):
        reset = (f"tu_cold_exit_offset[{succ_id}] = "
                 "TU_COLD_EXIT_INVALID")
        if recover.count(reset) != 1:
            errors.append(
                f"{name} must clear cold-exit successor {succ_id} once")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: RAS-independent TU direct-call cold-exit foundation")
