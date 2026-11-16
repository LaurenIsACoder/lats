#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Create a minimal uncompressed newc archive for the static benchmark init."""
import argparse
from pathlib import Path
import stat

parser = argparse.ArgumentParser()
parser.add_argument("elf", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
archive = bytearray()


def append(name, mode, payload=b"", major=0, minor=0):
    encoded = name.encode() + b"\0"
    fields = [1, mode, 0, 0, 1, 0, len(payload), 0, 0,
              major, minor, len(encoded), 0]
    archive.extend(b"070701" + b"".join(f"{v:08x}".encode() for v in fields))
    archive.extend(encoded)
    archive.extend(b"\0" * (-len(archive) % 4))
    archive.extend(payload)
    archive.extend(b"\0" * (-len(archive) % 4))


for directory in (".", "proc", "sys", "dev"):
    append(directory, stat.S_IFDIR | 0o755)
append("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
append("dev/null", stat.S_IFCHR | 0o666, major=1, minor=3)
append("init", stat.S_IFREG | 0o755, args.elf.read_bytes())
append("TRAILER!!!", 0)
archive.extend(b"\0" * (-len(archive) % 512))
args.output.write_bytes(archive)
