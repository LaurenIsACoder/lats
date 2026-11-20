#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile production mode parsing and high-bits gates in both TU builds."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
text = (root / 'target/i386/latx/latx-options.c').read_text()
header = (root / 'target/i386/latx/include/latx-options.h').read_text()
start = header.index('typedef enum LatxTuMode')
end = header.index('} LatxTuMode;', start) + len('} LatxTuMode;')
tu_modes = header[start:end]
start = header.index('static inline bool aot_tu_enabled(')
helper = header[start:header.index('\n}', start) + 2]
start = text.index('    opt = qemu_opt_find(opts, "tumode");')
parser = text[start:text.index('    parse_options_uint("tumaxtb"', start)]
start = text.index('static void parse_options_tu_mode_bit(')
mode_parser = text[start:text.index('\n}', start) + 2]
start = text.index('    latxs_options_check_sysflagtu();')
start = text.index('\n', start) + 1
end = min(text.index(marker, start) for marker in
          ('    if (option_tucp_fast', '    if (option_aot_cross_page_tu')
          if marker in text[start:])
checks = text[start:end]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int option_sys_aot;
static int option_aot_gpr_highbits, option_tu_gpr_highbits;
static int option_shbr, option_ghbr;
static unsigned option_tu_mode;
''' + tu_modes + r'''
static bool jit_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
static bool aot_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_AOT; }
#define LATXS_GHBR_IGNORE_MEMORY 2
#define LATXS_SHBR_IGNORE_MEMORY 2
#define LATXS_SHBR_AGGRESSIVE 3
typedef struct {
    const char *str;
    union { bool boolean; } value;
} QemuOpt, QemuOpts;
static QemuOpt *qemu_opt_find(QemuOpts *opts, const char *name)
{ (void)name; return opts->str ? opts : NULL; }
#define error_report(...) fprintf(stderr, __VA_ARGS__)
#define warn_report(...) fprintf(stderr, __VA_ARGS__)
''' + mode_parser + '\n' + helper + r'''
static void parse(QemuOpts *opts) {
    QemuOpt *opt;
''' + parser + r'''
}
static void parse_legacy(QemuOpts *tu, QemuOpts *aottu)
{
    parse_options_tu_mode_bit(tu, "tu", LATX_TU_MODE_JIT);
    parse_options_tu_mode_bit(aottu, "aottu", LATX_TU_MODE_AOT);
}
static bool tu_runtime_enabled(void)
{ return jit_tu_requested() || aot_tu_enabled(); }
static void check(void) {
''' + checks + r'''
}
int main(void) {
    QemuOpts on = { .str = "on", .value.boolean = true };
    QemuOpts off = { .str = "off" };
    QemuOpts absent = { 0 };

    option_tu_mode = 0;
    parse_legacy(&on, &absent);
    assert(jit_tu_requested() && !aot_tu_requested());
    option_tu_mode = 0;
    parse_legacy(&absent, &on);
    assert(!jit_tu_requested() && aot_tu_requested());
    option_tu_mode = 0;
    parse_legacy(&on, &on);
    assert(jit_tu_requested() && aot_tu_requested());
    parse_legacy(&off, &off);
    assert(!jit_tu_requested() && !aot_tu_requested());

    const char *modes[] = {"off", "jit", "aot", "both"};
    for (int mode = 0; mode < 4; mode++) {
        for (int sys = 0; sys < 2; sys++) {
            for (int aot = 0; aot < 2; aot++) {
                for (int jit = 0; jit < 2; jit++) {
                    QemuOpts opt = { .str = modes[mode] };
                    option_tu_mode = 0;
                    parse(&opt);
                    assert(jit_tu_requested() == (mode & 1));
                    assert(aot_tu_requested() == ((mode >> 1) & 1));
                    option_sys_aot = sys;
                    option_aot_gpr_highbits = aot;
                    option_tu_gpr_highbits = jit;
                    bool expect_aot = false;
#ifdef CONFIG_LATX_TU
                    expect_aot = aot && sys && (mode & 2);
#endif
                    bool expect_jit = jit && jit_tu_requested();
                    check();
                    assert(option_aot_gpr_highbits == expect_aot);
                    assert(option_tu_gpr_highbits == expect_jit);
                }
            }
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='aot-tu-mode-') as work:
    for enabled in (False, True):
        binary = str(Path(work) / 'test')
        command = [os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror']
        if enabled:
            command += ['-DCONFIG_LATX_TU']
        subprocess.run(command + ['-x', 'c', '-', '-o', binary],
                       input=source, text=True, check=True)
        subprocess.run([binary], capture_output=True, check=True)
        print(f'PASS: 32 mode/high-bits combinations, CONFIG_LATX_TU={enabled}')
