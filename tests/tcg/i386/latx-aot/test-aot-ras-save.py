#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Saving closes live gates even without assertions; preserves disabled sites."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
def fragment(path, begin):
    source = (root / path).read_text()
    start = source.index(begin)
    return source[start:source.index("#if DEBUG_AOT", start)]
plain = fragment("target/i386/latx/sbt/sys-aot-save.c",
                 "        if (option_rastoptp && tb->inst_ptr_offset)")
tu = fragment("target/i386/latx/sbt/sys-aot-tu-codec.c",
              "        if (option_soft_ras && tb->inst_ptr_offset")
program = r'''
#include <assert.h>
#include <stdbool.h>
#define MF_RAS_CLEAR_B0 0x400
#define MF_RAS_LINKED 0x800
#define qatomic_read(p) (*(p))
#ifdef G_DISABLE_ASSERT
#define g_assert(x) ((void)0)
#else
#define g_assert(x) assert(x)
#endif
typedef struct { int inst_ptr_offset, misc_flags; bool gate_open; } TranslationBlock;
static int option_rastoptp=1, option_soft_ras=1, calls;
static bool close_ok=true, permanent_ok=true;
static bool latxs_ras_tp_permanent_valid(TranslationBlock *tb)
{ return permanent_ok && !tb->gate_open && (tb->misc_flags & MF_RAS_CLEAR_B0); }
static bool latxs_ras_close_relinkable(TranslationBlock *tb)
{
    calls++;
    if (!close_ok || tb->misc_flags & MF_RAS_CLEAR_B0) return false;
    tb->misc_flags &= ~MF_RAS_LINKED;
    tb->gate_open = false;
    return true;
}
static void latxs_ras_unlink(TranslationBlock *tb)
{ tb->misc_flags = MF_RAS_CLEAR_B0; tb->gate_open = false; }
static bool save_plain(TranslationBlock *tb)
{
''' + plain + r'''
    return true;
fail: return false;
}
static bool save_tu(TranslationBlock *tb)
{
''' + tu + r'''
    return true;
out: return false;
}
int main(void)
{
    TranslationBlock tb = {16, MF_RAS_LINKED, true};
    assert(save_plain(&tb) && calls==1 && !tb.gate_open && !tb.misc_flags);
    tb = (TranslationBlock){16, MF_RAS_CLEAR_B0, false}; calls=0;
    assert(save_plain(&tb) && calls==0 && !tb.gate_open && tb.misc_flags==MF_RAS_CLEAR_B0);
    tb = (TranslationBlock){16, MF_RAS_LINKED, true}; close_ok=false;
    assert(!save_plain(&tb) && calls==1 && tb.gate_open);
    close_ok=true; calls=0;
    assert(save_tu(&tb) && calls==1 && !tb.gate_open && !tb.misc_flags);
    tb = (TranslationBlock){16, MF_RAS_LINKED, true}; close_ok=false; calls=0;
    assert(!save_tu(&tb) && calls==1 && tb.gate_open);
    close_ok=true; calls=0;
    tb=(TranslationBlock){16,MF_RAS_CLEAR_B0,false};
    assert(save_tu(&tb) && calls==0 && tb.misc_flags==MF_RAS_CLEAR_B0);
    permanent_ok=false;
    assert(!save_tu(&tb) && !save_plain(&tb) && calls==0);
    option_rastoptp=0; calls=0;
    assert(save_plain(&tb) && calls==0 && !tb.gate_open && tb.misc_flags==MF_RAS_CLEAR_B0);
}
'''
with tempfile.TemporaryDirectory(prefix="aot-ras-save-") as directory:
    for disabled in (False, True):
        binary=Path(directory)/("test-"+str(disabled))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror"] +
                       (["-DG_DISABLE_ASSERT"] if disabled else []) +
                       ["-x", "c", "-o", str(binary), "-"],
                       input=program, text=True, check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: live AOT gates close with assertions on/off; disabled sites remain closed")
