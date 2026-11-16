#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build one correctness/performance fixture with an explicit target compiler."""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("fixture", choices=("direct", "indirect", "chain", "fp", "multi", "smc"))
parser.add_argument("--arch", choices=("x86_64", "loongarch64"), required=True)
parser.add_argument("--cc", default="cc")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
kit = Path(__file__).resolve().parent.parent
args.out.mkdir(parents=True, exist_ok=True)
source = kit / args.fixture
if args.fixture == "chain":
    source = args.out / "chain-source"
    subprocess.run([sys.executable, str(kit / "scripts/generate-chain.py"),
                    str(source)], check=True)
common = kit / "common"
sources = [common / "main.c", source / "kernels.c"]
if args.fixture in ("direct", "chain"):
    sources.append(source / "callees.c")
binary = args.out / ("bench-x86" if args.arch == "x86_64" else "bench-la")
flags = ["-O2", "-static", "-no-pie", "-I", str(common)]
if args.arch == "x86_64":
    flags.append("-mno-red-zone")
subprocess.run(shlex.split(args.cc) + flags + [str(p) for p in sources] +
               ["-o", str(binary)], check=True)
if args.arch == "x86_64":
    subprocess.run([sys.executable, str(kit / "scripts/create-initrd.py"),
                    str(binary), str(args.out / "bench.cpio")], check=True)
