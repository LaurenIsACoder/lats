#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build old/new production output loops with native softmmu target flags."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=Path)
p.add_argument("baseline", type=Path, help="directory with baseline-sys-translate.c and baseline-sys-tu.c")
p.add_argument("output", type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
test = Path(__file__).resolve().parent
a.build = a.build.resolve()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
entry = next(e for e in json.loads((a.build / "compile_commands.json").read_text())
             if e["file"].endswith("/sys-translate.c"))
args = shlex.split(entry["command"])
flags = []
i = 0
while i < len(args):
    if args[i] in ("-o", "-c", "-MF", "-MQ", "-MT"):
        i += 2
    elif args[i] == "-MD":
        i += 1
    else:
        flags.append(args[i])
        i += 1
flags += ["-ffunction-sections", "-fdata-sections", "-I", str(test)]
prefix = '#include "output-support.h"\n'
sources = []
for variant in ["base", "new"]:
    tbfile = a.baseline / "baseline-sys-translate.c" if variant == "base" else root / "target/i386/latx/translator/sys-translate.c"
    tufile = a.baseline / "baseline-sys-tu.c" if variant == "base" else root / "target/i386/latx/optimization/sys-tu.c"
    tb, tu = tbfile.read_text(), tufile.read_text()
    sources += [tbfile, tufile]
    start = "inline int latxs_tr_ir2_assemble(" if variant == "base" else "static inline __attribute__((always_inline))\nint latxs_tr_ir2_emit("
    body = tb[tb.index(start):tb.index("\nint latxs_tr_translate_tb(")]
    body = body.replace("inline int latxs_tr_ir2_assemble(", f"int {variant}_tb(")
    (a.output / f"{variant}-tb.c").write_text(prefix + body)
    body = tu[tu.index("static inline int fix_tb_info("):tu.index("\n/* static void set_pg_mode_")]
    body = body.replace("latxs_tr_ir2_assemble_in_tu", f"{variant}_tu_impl")
    variables = tu[tu.index("static __thread int search_id;"):tu.index("\nstatic int16_t tu_ir1_index")]
    ras = tu[tu.index("typedef struct RasTPCallLabels {"):tu.index("} RasTPCallLabels;") + len("} RasTPCallLabels;")]
    hook = '''
static int latxs_label_dispose_in_tu(TranslationBlock **list,
        const RasTPCallLabels *ras, void *const *direct, bool inline_hras)
{
    (void)ras; (void)direct; (void)inline_hras;
    return output_labels_tu(list);
}
'''
    wrappers = f'''
int {variant}_tu(TranslationBlock **list)
{{
    search_id = search_size = 0;
    return {variant}_tu_impl(list, NULL, NULL, false);
}}
int {variant}_search(uint8_t *dest, uint32_t *offsets)
{{
    memcpy(dest, search_buff, search_size);
    memcpy(offsets, search_buff_offset, search_id * sizeof(*offsets));
    return search_size;
}}
'''
    (a.output / f"{variant}-tu.c").write_text(prefix + ras + "\n" + variables + hook + body + wrappers)
tb = (a.baseline / "baseline-sys-translate.c").read_text()
overflow = tb[tb.index("int latxs_tr_check_buffer_overflow("):tb.index("\ninline int latxs_tr_ir2_assemble(")]
(a.output / "overflow.c").write_text(prefix + overflow)
search = root / "target/i386/latx/translator/sys-excp.c"
text = search.read_text()
(a.output / "search.c").write_text(prefix + text[text.index("static uint8_t *encode_sleb128("):])
encoder = root / "target/i386/latx/ir2/ir2-assemble.c"
text = encoder.read_text()
(a.output / "encoder.c").write_text('#include "qemu/osdep.h"\n' + text[:text.index("\nstatic\nvoid latxs_ir2_op_check_imm")])
sources += [search, encoder, test / "output.c", test / "output-support.h"]
for profile in [False, True]:
    objects = []
    options = flags + (["-DCONFIG_LATX_PROFILER=1"] if profile else [])
    for src in sorted(a.output.glob("*.c")) + [test / "output.c"]:
        obj = a.output / (src.stem + ("-profile.o" if profile else ".o"))
        subprocess.run(options + ["-c", str(src), "-o", str(obj)], cwd=entry["directory"], check=True)
        objects.append(str(obj))
    glib = shlex.split(subprocess.check_output(["pkg-config", "--libs", "glib-2.0"], text=True))
    binary = a.output / ("output-profile" if profile else "output")
    subprocess.run(options + ["-no-pie", "-Wl,--gc-sections", "-o", str(binary)] + objects + glib + ["-lpthread", "-lm"], cwd=entry["directory"], check=True)
(a.output / "manifest.json").write_text(json.dumps({
    "sha256": {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sources},
    "compile_flags": flags, "build_directory": entry["directory"]}, indent=2) + "\n")
print(a.output)
