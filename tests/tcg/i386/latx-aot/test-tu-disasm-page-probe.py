#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check JIT TU cross-page probes and CET incsspd/incsspq cross-page boundary guards."""
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[4]
path = "target/i386/latx/translator/sys-disasm.c"
source = (
    subprocess.check_output(["git", "show", ":" + path], cwd=root).decode()
    if "--index" in sys.argv else (root / path).read_text()
)


def extract_fn(src: str, name: str) -> str:
    pos = 0
    while True:
        idx = src.index(name + "(", pos)
        close = src.index(")", idx)
        rest = src[close + 1 :].lstrip()
        if rest.startswith("{"):
            start = src.rfind("\n", 0, idx) + 1
            brace = src.index("{", close)
            depth, end = 1, brace + 1
            while depth:
                depth += (src[end] == "{") - (src[end] == "}")
                end += 1
            return src[start:end]
        pos = close + 1


probe_start = source.index("static void latxs_before_inst_fetch(")
probe_end = source.index("/* Function inside X86toMIPS", probe_start)
probe = source[probe_start:probe_end]
assert "get_page_addr_code_hostp_noexit(" in probe

failure = probe[probe.index("if (phys_page2_new == -1)"):]
failure = failure[:failure.index("phys_page2_old =")]
assert "if (td->is_tu_mode)" in failure
assert "td->deadlock_avoidance = -4;" in failure
assert "td->is_tu_mode && cp_tu_pages()" not in failure

read_start = source.index("uint8_t latxs_cpu_read_code_via_qemu(")
read_end = source.index("\n}", read_start)
read_code = source[read_start:read_end]
guard = read_code.index("if (lsenv->tr_data->is_tu_mode &&")
assert "lsenv->tr_data->deadlock_avoidance" in read_code[guard:]
load = read_code.index("cpu_ldub_code(")
assert guard < load

disasm_start = source.index("static inline int ir1_is_incsspd(")
disasm_end = source.index("#define LATXS_X86_MAX_INSN_LENGTH", disasm_start)
disasm_code = source[disasm_start:disasm_end]
assert "ir1_is_incssp_rex(addr)" in disasm_code
assert "ir1_is_incsspd(addr)" in disasm_code

err_start = source.index("static void __latxs_disasm_do_error_sys(")
err_end = source.index("static ADDRX aot_get_hva(", err_start)
err_code = source[err_start:err_end]
assert "ir1_is_rdssp_rex(inst_cache) || ir1_is_incssp_rex(inst_cache)" in err_code
assert "ir1_is_rdsspd(inst_cache) || ir1_is_incsspd(inst_cache)" in err_code

one_start = source.index("static inline ADDRX __latxs_disasm_one_ir1(")
one_end = source.index("void latxs_tr_disasm(", one_start)
one_code = source[one_start:one_end]
assert "uint8_t inst[64] = { 0 };" in one_code
assert "ir1_0fae_missing_modrm(bytes, available," in one_code
assert "ir1_0fae_missing_modrm(inst, len," in one_code
assert "int nop_size = ir1_gennop_size(inst);" in one_code
assert "if (nop_size && nop_size <= len)" in one_code

program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    struct { bool code64; } sys;
} TRANSLATION_DATA;
typedef struct { TRANSLATION_DATA *tr_data; } ENV;
static TRANSLATION_DATA td_state;
static ENV env_state = { .tr_data = &td_state };
static ENV *lsenv = &env_state;

static int ir1_is_rdsspd(uint8_t *p) { (void)p; return 0; }
static int ir1_is_rdssp_rex(uint8_t *p) { (void)p; return 0; }
"""
    + "\n"
    + extract_fn(source, "ir1_is_incsspd")
    + "\n"
    + extract_fn(source, "ir1_is_incssp_rex")
    + "\n"
    + extract_fn(source, "is_prefix")
    + "\n"
    + extract_fn(source, "ir1_0fae_missing_modrm")
    + "\n"
    + extract_fn(source, "ir1_gennop_size")
    + r"""
int main(void)
{
    /* Truncated 0F AE right at page end (missing ModRM on next page) must fail closed. */
    assert(ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x0f, 0xae, 0x00 }, 3, true));
    assert(ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x0f, 0xae, 0x00 }, 3, false));
    assert(ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x48, 0x0f, 0xae, 0x00 }, 4, true));
    assert(!ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x48, 0x0f, 0xae, 0x00 }, 4, false));
    assert(ir1_0fae_missing_modrm((uint8_t[]){ 0x0f, 0xae, 0x00 }, 2, true));

    /* Complete in-page lfence (0F AE E8) and complete incsspd/incsspq are NOT missing ModRM. */
    assert(!ir1_0fae_missing_modrm((uint8_t[]){ 0x0f, 0xae, 0xe8 }, 3, true));
    assert(!ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x0f, 0xae, 0xe8 }, 4, true));
    assert(!ir1_0fae_missing_modrm((uint8_t[]){ 0xf3, 0x48, 0x0f, 0xae, 0xe8 }, 5, true));

    /* Full byte sequences decode to 4-byte incsspd / 5-byte incsspq GENNOP sizes. */
    td_state.sys.code64 = true;
    assert(ir1_gennop_size((uint8_t[]){ 0xf3, 0x0f, 0xae, 0xe8 }) == 4);
    assert(ir1_gennop_size((uint8_t[]){ 0xf3, 0x48, 0x0f, 0xae, 0xef }) == 5);
    td_state.sys.code64 = false;
    assert(ir1_gennop_size((uint8_t[]){ 0xf3, 0x0f, 0xae, 0xe8 }) == 4);
    assert(ir1_gennop_size((uint8_t[]){ 0xf3, 0x48, 0x0f, 0xae, 0xef }) == 0);
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-0fae-probe-") as folder:
    binary = Path(folder) / "check"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-x", "c", "-o", str(binary), "-"],
        input=program,
        text=True,
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("PASS: JIT TU page probes and CET 0F AE cross-page guards verified")
