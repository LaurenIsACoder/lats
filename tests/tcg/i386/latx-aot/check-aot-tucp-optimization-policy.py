#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import sys


root = Path(__file__).resolve().parents[4]
options = (root / "target/i386/latx/latx-options.c").read_text()
fmt = (root / "target/i386/latx/sbt/sys-aot-format.c").read_text()
save = (root / "target/i386/latx/sbt/sys-aot-tu-codec.c").read_text()
load = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
tu = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
config = (root / "target/i386/latx/latx-config.c").read_text()
cross_page = (
    root / "target/i386/latx/optimization/sys-cross-page.c"
).read_text()
errors = []


if "option_sys_aot && tucp_fast()" in options:
    errors.append("system AOT still disables tucpfast globally")
if "bool aot_tucp = aot_cross_page_tu_enabled();" not in options:
    errors.append("tucpfast validation does not accept AOT-only cross-page TU")
if "(!jit_tucp && !aot_tucp)" not in options:
    errors.append("tucpfast readiness does not distinguish JIT and AOT TU")

for token, description in (
    ("tail.aot_tu_codegen_flags = get_aot_tu_codegen_flags(aot_mode)",
     "saved AOT TU code-generation identity"),
    ("tail->aot_tu_codegen_flags != get_aot_tu_codegen_flags(aot_mode)",
     "loaded AOT TU code-generation identity check"),
    ("option_tucp_fast & AOT_TU_CODEGEN_FAST_MASK",
     "tucpfast identity bits"),
    ("option_tucp_generation & AOT_TU_CODEGEN_GEN_MASK",
     "tucpgen identity bits"),
):
    if token not in fmt:
        errors.append(f"AOT format lacks {description}")

for index in (0, 1):
    save_token = (
        f"aot_tb->tu_generation_offset_patch[{index}] =\n"
        f"        tb->s_data->tu_generation_offset_patch[{index}]"
    )
    load_token = (
        f"curr_tb->s_data->tu_generation_offset_patch[{index}] =\n"
        f"        aot_tb->tu_generation_offset_patch[{index}]"
    )
    if save_token not in save:
        errors.append(f"AOT save drops tucpgen patch slot {index}")
    if load_token not in load:
        errors.append(f"AOT load drops tucpgen patch slot {index}")

if "desc->id = tu_pages_alloc_id();" not in load:
    errors.append("loaded AOT TU does not receive a generation-cache ID")
if "uint32_t tu_pages_alloc_id(void)" not in tu:
    errors.append("TU generation-cache ID allocator is not shared with AOT")
if "tu_head->page_count > 1 || tucp_fast_needs_pages_desc()" not in load:
    errors.append("single-page AOT TUs cannot participate in tucpfast links")
if "hamt_enabled() && hamt_mode_dmw() ?" not in config:
    errors.append(
        "non-HAMT generation arrays can receive an unusable DMW address")
if "BSTRPICK_D, tmp1, tmp1, 52, 12" in cross_page:
    errors.append(
        "fast iTLB PTW treats x86 PTE software bit 52 as an address bit")
if cross_page.count("BSTRPICK_D, tmp1, tmp1, 51, 12") != 3:
    errors.append(
        "fast iTLB PTW does not mask all non-CR3 page-table addresses")
if "BSTRPICK_D,  tmp1, tmp1, 51, 12" not in cross_page:
    errors.append("fast iTLB PTW does not mask CR3 with PG_ADDRESS_MASK")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: AOT TU preserves tucpfast/tucpgen metadata and runtime IDs")
