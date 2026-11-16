#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build the real old/new EOB functions with a configured softmmu build's flags."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=Path, help="native x86_64-softmmu build directory")
p.add_argument("baseline", type=Path, help="unmodified sys-eob.c")
p.add_argument("output", type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
a.build = a.build.resolve()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
entry = next(e for e in json.loads((a.build / "compile_commands.json").read_text())
             if e["file"].endswith("/sys-eob.c"))
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
# The excerpt also contains static helpers used only by the emitter below it.
flags += ["-ffunction-sections", "-fdata-sections", "-Wno-unused-function"]
candidate = root / "target/i386/latx/translator/sys-eob.c"
symbols = ["latxs_ir1_is_cross_page", "latxs_ir1_is_cross_page_eip",
           "latxs_ir1_is_mov_to_seg_ss", "latxs_ir1_is_pop_to_seg_ss",
           "latxs_ir1_is_eob_in_sys"]
objects = []
for name, source in [("base", a.baseline), ("new", candidate)]:
    text = source.read_text()
    text = text[:text.index("\nvoid latxs_tr_gen_eob(void)")]
    defines = "".join(f"#define {s} {name}_{s}\n" for s in symbols)
    generated = a.output / f"eob-{name}.c"
    generated.write_text(defines + '#include "qemu/osdep.h"\n' + text)
    objects.append((generated, a.output / f"eob-{name}.o"))
for source in [Path(__file__).with_name("bench.c"),
               root / "target/i386/latx/ir1/ir1.c"]:
    objects.append((source, a.output / (source.stem + ".o")))
for source, obj in objects:
    subprocess.run(flags + ["-c", str(source), "-o", str(obj)],
                   cwd=entry["directory"], check=True)
glib = shlex.split(subprocess.check_output(
    ["pkg-config", "--libs", "glib-2.0"], text=True))
subprocess.run(flags + ["-no-pie", "-Wl,--gc-sections", "-o",
                str(a.output / "bench")] + [str(o) for _, o in objects] +
               [str(a.build / "target/i386/latx/libcapStone.a")] +
               glib + ["-lpthread", "-lm"], check=True)
sources = [a.baseline, candidate, Path(__file__).with_name("bench.c"),
           root / "target/i386/latx/ir1/ir1.c", root / "target/i386/latx/include/ir1.h"]
(a.output / "manifest.json").write_text(json.dumps({
    "sha256": {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sources},
    "compile_flags": flags, "build_directory": entry["directory"]}, indent=2) + "\n")
print(a.output / "bench")
