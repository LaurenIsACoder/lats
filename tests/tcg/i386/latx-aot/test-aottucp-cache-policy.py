#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run production flag generation/integrity checks without starting a VM."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-format.c").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


flags = function("static uint64_t get_option_flags(")
integrity = function("bool test_integrity(")
save_tail = function("bool save_aot_tail(")
read_tail = function("aot_tail *get_aot_tail(").replace(
    "get_aot_tail(int fd)", "read_disk_tail(int fd)")
codegen = (function("static uint32_t get_aot_tu_codegen_flags(")
           if "static uint32_t get_aot_tu_codegen_flags(" in source else "")
defines = "\n".join(re.findall(r"^#define .*", source, re.M))
options = sorted(set(re.findall(r"\boption_\w+", flags + integrity + codegen)) |
                 {"option_aot_cross_page_tu", "option_pcid_asid"})
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t uint64;
#define AOT_USER 2
#define AOT_CORE 1
#define AOT_TU_FLAG_BIT 10
#define LATXS_GHBR_DISABLED 0
#define LATXS_GHBR_IGNORE_MEMORY 2
#define AOT_VERSION "test-version"
#define LOAD_HELPER_BEGIN 0
#define LOAD_HELPER_END 1
static unsigned option_tu_mode;
static int option_sys_aot = 1;
#define LATX_TU_MODE_JIT (1u << 0)
#define LATX_TU_MODE_AOT (1u << 1)
static bool jit_tu_enabled(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
static bool aot_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_AOT; }
static bool aot_tu_enabled(void)
{ return option_sys_aot && aot_tu_requested(); }
static uint64_t relkind_to_fixup_addr[2];
static int pre_file_tb_count;
typedef struct aot_tail {
    char aot_version[64];
    uint64_t flags, helper_addr[2];
    uint32_t sys_flag_reduction_tu, aot_tu_codegen_flags, njc_bits;
    uint32_t interrupt_mode;
    int page_num, tb_num;
} aot_tail;
static aot_tail saved;
static FILE *out_tb_file;
static aot_tail *read_disk_tail(int fd);
#define LATX_JMP_CACHE_HASH_PAGE 0
#define LATX_JMP_CACHE_HASH_FULL 1
#define LATX_JMP_CACHE_HASH_LINE 2
static bool legacy_inline, compact;
static bool intb_njc_inline_enabled(void) { return legacy_inline; }
static bool latx_jmp_cache_compact_enabled(void) { return compact; }
static uint32_t latx_jmp_cache_bits(void) { return 16; }
static aot_tail *get_aot_tail(int fd) {
    if (fd >= 0) return read_disk_tail(fd);
    aot_tail *copy = malloc(sizeof(*copy));
    assert(copy);
    *copy = saved;
    return copy;
}
'''
main = r'''
int main(void) {
    strcpy(saved.aot_version, AOT_VERSION);
    for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
        for (int before = 0; before <= 3; before++) {
            option_int_opt = before;
            saved.interrupt_mode = before;
            saved.flags = get_option_flags(mode);
            saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
            for (int after = 0; after <= 3; after++) {
                option_int_opt = after;
                assert(test_integrity(-1, mode) == (before == after));
            }
        }
    }
    option_int_opt = saved.interrupt_mode = 0;
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int before = 0; before <= 1; before++) {
                option_aot_cross_page_tu = before;
                saved.flags = get_option_flags(mode);
                saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                for (int after = 0; after <= 1; after++) {
                    option_aot_cross_page_tu = after;
                    bool scoped = HAVE_TU && tu && mode == AOT_USER;
                    assert(test_integrity(-1, mode) ==
                           (!scoped || before == after));
                }
            }
            /* Old files have neither policy bit, even if saved with CP on. */
            option_aot_cross_page_tu = 0;
            saved.flags = get_option_flags(mode) &
                ~((UINT64_C(1) << AOT_TU_CROSS_PAGE_BIT) |
                  (UINT64_C(1) << AOT_TU_CROSS_PAGE_POLICY_BIT));
            saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
            for (int after = 0; after <= 1; after++) {
                option_aot_cross_page_tu = after;
                assert(test_integrity(-1, mode) ==
                       !(HAVE_TU && tu && mode == AOT_USER));
            }
        }
    }
    /* Inline DMW helper returns are present in both core and user TBs. */
    for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
        for (int before = 0; before <= 1; before++) {
            option_pcid_asid = before;
            out_tb_file = tmpfile();
            assert(out_tb_file && save_aot_tail(1, 1, mode));
            assert(fflush(out_tb_file) == 0);
            int fd = fileno(out_tb_file);
            assert(test_integrity(fd, mode));
            option_pcid_asid = !before;
            assert(!test_integrity(fd, mode));
            fclose(out_tb_file);
        }
    }
    option_pcid_asid = 0;
    /* Saved fast-iTLB instructions differ with the PCID bank layout. */
    for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
        for (int before = 0; before <= 1; before++) {
            option_pcid_cache = before;
            out_tb_file = tmpfile();
            assert(out_tb_file && save_aot_tail(1, 1, mode));
            assert(fflush(out_tb_file) == 0);
            int fd = fileno(out_tb_file);
            assert(test_integrity(fd, mode));
            option_pcid_cache = !before;
            assert(!test_integrity(fd, mode));
            fclose(out_tb_file);
        }
    }
    option_pcid_cache = 0;
    /* Changing the IRQ layout changes only saved user TU code. */
    option_rastoptp = 0;
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int before = 0; before <= 1; before++) {
                option_njc_source_jump = before ? 2 : 0;
                saved.flags = get_option_flags(mode);
                saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                assert(test_integrity(-1, mode));
                option_njc_source_jump = before ? 0 : 2;
                assert(test_integrity(-1, mode) ==
                       !(HAVE_TU && tu && mode == AOT_USER));
            }
        }
    }
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int enabled = 0; enabled <= 1; enabled++) {
                option_njc_source_jump = enabled ? 2 : 0;
                for (int before = 0; before <= 1; before++) {
                    option_table_addr = before;
                    saved.flags = get_option_flags(mode);
                    saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                    option_table_addr = !before;
                    assert(test_integrity(-1, mode) ==
                           !(HAVE_TU && tu && mode == AOT_USER && enabled));
                }
            }
        }
    }
    option_table_addr = 0;
    option_njc_source_jump = 0;
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int before = 0; before <= 1; before++) {
                option_irq_backedge = before;
                saved.flags = get_option_flags(mode);
                saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                assert(test_integrity(-1, mode));
                option_irq_backedge = !before;
                assert(test_integrity(-1, mode) ==
                       !(HAVE_TU && tu && mode == AOT_USER));
            }
        }
    }
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            bool scoped = HAVE_TU && mode == AOT_USER;

            saved.flags = get_option_flags(mode);
            for (int fast = 0; fast < 64; fast++) {
                for (int gen = 0; gen < 4; gen++) {
                    option_tucp_fast = fast;
                    option_tucp_generation = gen;
                    saved.aot_tu_codegen_flags =
                        get_aot_tu_codegen_flags(mode);
                    assert(test_integrity(-1, mode));
                    for (int bit = 0; bit < 6; bit++) {
                        option_tucp_fast = fast ^ (1 << bit);
                        assert(test_integrity(-1, mode) == !scoped);
                    }
                    option_tucp_fast = fast;
                    for (int other = 0; other < 4; other++) {
                        option_tucp_generation = other;
                        assert(test_integrity(-1, mode) ==
                               (!scoped || gen == other));
                    }
                }
            }
            /* Ordinary legacy files did not identify fast/gen settings. */
            if (!tu) {
                option_tucp_fast = option_tucp_generation = 0;
                saved.aot_tu_codegen_flags = 0;
                assert(test_integrity(-1, mode) == !scoped);
            }
        }
    }
    option_aot_cross_page_tu = 0;
    option_tucp_fast = 0;
    option_tucp_generation = 0;
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int before = LATXS_GHBR_DISABLED;
                 before <= LATXS_GHBR_IGNORE_MEMORY; before++) {
                option_ghbr = before;
                saved.flags = get_option_flags(mode);
                saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                if (HAVE_TU && mode == AOT_USER && before != 0) {
                    uint32_t current = saved.aot_tu_codegen_flags;

                    saved.aot_tu_codegen_flags &=
                        ~(AOT_GHBR_LEVEL_MASK << AOT_GHBR_LEVEL_SHIFT);
                    assert(!test_integrity(-1, mode));
                    saved.aot_tu_codegen_flags = current;
                }
                for (int after = LATXS_GHBR_DISABLED;
                     after <= LATXS_GHBR_IGNORE_MEMORY; after++) {
                    option_ghbr = after;
                    assert(test_integrity(-1, mode) ==
                           (!HAVE_TU || mode == AOT_CORE || before == after));
                }
            }
        }
    }
    /* Plain TB, TU and core caches must not mix the two r2 conventions. */
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int before = 0; before <= 1; before++) {
                option_rastoptp = before;
                saved.flags = get_option_flags(mode);
                saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                assert(test_integrity(-1, mode));
                option_rastoptp = !before;
                assert(!test_integrity(-1, mode));
            }
        }
    }
    /* Shared/leaf queries use the current runtime hash; saved inline code
     * must identify its hash, including the source-JMP path with njcinline=0. */
    option_hamt = option_intb_njc = option_intb_link = option_flaged_sht = 1;
    option_rastoptp = option_soft_ras = 1;
    enum { SHARED, LEAF, SOURCE_INLINE, LEGACY_INLINE };
    compact = true;
    for (int tu = 0; tu <= 1; tu++) {
        option_tu_mode = tu ? LATX_TU_MODE_AOT : 0;
        for (int mode = AOT_CORE; mode <= AOT_USER; mode++) {
            for (int path = SHARED; path <= LEGACY_INLINE; path++) {
                legacy_inline = path == LEGACY_INLINE;
                option_table_addr = path != LEAF;
                option_njc_source_jump = HAVE_TU && tu &&
                    (path == LEAF || path == SOURCE_INLINE) ? 2 : 0;
                bool stored_hash = legacy_inline ||
                    (HAVE_TU && tu && mode == AOT_USER && path == SOURCE_INLINE);
                for (int before = LATX_JMP_CACHE_HASH_PAGE;
                     before <= LATX_JMP_CACHE_HASH_LINE; before++) {
                    option_jmp_cache_hash = before;
                    saved.flags = get_option_flags(mode);
                    saved.aot_tu_codegen_flags = get_aot_tu_codegen_flags(mode);
                    saved.njc_bits = before == LATX_JMP_CACHE_HASH_PAGE ? 0 : 16;
                    for (int after = LATX_JMP_CACHE_HASH_PAGE;
                         after <= LATX_JMP_CACHE_HASH_LINE; after++) {
                        option_jmp_cache_hash = after;
                        bool compatible = before == after ||
                            (!stored_hash && before != LATX_JMP_CACHE_HASH_PAGE &&
                             after != LATX_JMP_CACHE_HASH_PAGE);
                        assert(test_integrity(-1, mode) == compatible);
                    }
                }
            }
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aottucp-policy-") as directory:
    path = Path(directory)
    test = path / "test.c"
    test.write_text(prefix + defines + "\n" +
                    "\n".join(f"static int {name};" for name in options) +
                    "\n" + codegen + flags + read_tail + save_tail + integrity + main)
    for enabled in (0, 1):
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", f"-DHAVE_TU={enabled}"]
        if enabled:
            command += ["-DCONFIG_LATX_TU"]
        subprocess.run(command + [str(test), "-o", str(path / "test")],
                       check=True)
        subprocess.run([str(path / "test")], check=True, capture_output=True)
        print(f"PASS: pcidasid saved core/user tail identity; TU={enabled}")
        print("PASS: aottucp transitions, legacy rejection, scope; "
              f"TU={enabled}")
        print(f"PASS: TB/TU fast/gen identity and legacy policy; TU={enabled}")
        print(f"PASS: GHBR AOT identity scope and levels; TU={enabled}")
        print(f"PASS: PAGE/FULL/LINE identity for shared, leaf and inline NJC; TU={enabled}")
