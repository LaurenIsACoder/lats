#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the 1M SMC regression in an isolated Linux initramfs."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def member(name, data, mode, inode, major=0, minor=0):
    name = name.encode() + b"\0"
    fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0,
              major, minor, len(name), 0]
    header = b"070701" + b"".join(f"{field:08x}".encode() for field in fields)
    result = header + name
    result += bytes(-len(result) % 4)
    result += data
    return result + bytes(-len(data) % 4)


def check_guest(data):
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("guest must be a little-endian ELF64 executable")
    if struct.unpack_from("<H", data, 16)[0] not in (2, 3):
        raise ValueError("guest must be an ELF executable")
    if struct.unpack_from("<H", data, 18)[0] != 62:
        raise ValueError("guest must target x86-64")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    if not phnum or phentsize < 56 or phoff + phentsize * phnum > len(data):
        raise ValueError("invalid ELF program headers")
    for i in range(phnum):
        if struct.unpack_from("<I", data, phoff + phentsize * i)[0] == 3:
            raise ValueError("guest must be statically linked")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "bios", "kernel", "guest", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--latx", default="select,verbose=on,aotsys=0,tumode=jit,tucpopt=max")
    parser.add_argument("--timeout", default=180, type=int)
    args = parser.parse_args()
    guest = args.guest.read_bytes()
    check_guest(guest)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive = member("init", guest, 0o100755, 1)
    archive += member("dev", b"", 0o40755, 2)
    archive += member("dev/console", b"", 0o20600, 3, 5, 1)
    archive += member("proc", b"", 0o40755, 4)
    archive += member("TRAILER!!!", b"", 0, 5)
    initrd = output / "guest.cpio"
    initrd.write_bytes(archive)
    command = [str(args.binary.resolve()), "-L", str(args.bios.resolve()),
               "-m", "4096", "-smp", "1", "-nographic", "-monitor", "none",
               "-kernel", str(args.kernel.resolve()), "-initrd", str(initrd),
               "-append", "console=ttyS0 rdinit=/init transparent_hugepage=always norandmaps",
               "-latx", args.latx]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    log = output / "vm.log"
    with log.open("wb") as stream:
        try:
            completed = subprocess.run(command, stdin=subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT,
                                       timeout=args.timeout, check=False)
        except subprocess.TimeoutExpired:
            raise RuntimeError(f"guest timed out; see {log}") from None
    text = log.read_text(errors="replace")
    lines = set(text.splitlines())
    required = ("CONTROL_255_PASS", "EXEC iteration=1 pages=256",
                "EXEC iteration=2 pages=256", "EXEC iteration=3 pages=256",
                "HAMT_BOUNDARY_RESULT=PASS")
    if (completed.returncode != 0 or any(marker not in lines for marker in required)
            or "reboot: Power down" not in text
            or "HAMT_BOUNDARY_RESULT=FAIL" in text or "Kernel panic" in text):
        raise RuntimeError(f"guest failed (exit {completed.returncode}); see {log}")
    identity = {name + "_sha256": sha256(path.read_bytes()) for name, path in
                (("binary", args.binary), ("kernel", args.kernel), ("guest", args.guest))}
    identity.update(result="PASS", latx=args.latx, exit_code=completed.returncode)
    (output / "result.json").write_text(json.dumps(identity, indent=2) + "\n")
    print("PASS: 255-page control and 256-page SMC regression; see", log)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
