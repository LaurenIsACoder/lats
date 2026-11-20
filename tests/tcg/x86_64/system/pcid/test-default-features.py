#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check qemu64 PCID defaults and explicit feature overrides through QMP."""

import json
import subprocess
import sys


def run_qmp(binary, latx, model, props=None, verbose=False):
    commands = [
        {"execute": "qmp_capabilities"},
        {"execute": "query-cpu-model-expansion", "arguments": {
            "type": "full", "model": {"name": model, "props": props or {}}}},
        {"execute": "quit"},
    ]
    proc = subprocess.Popen(
        [binary, "-machine", "none", "-accel", "tcg,mode=lat", "-nodefaults",
         "-display", "none", "-qmp", "stdio", "-latx", latx],
        stdin=subprocess.PIPE, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    try:
        stdout, stderr = proc.communicate(
            "".join(json.dumps(c) + "\n" for c in commands), timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        raise
    replies = [json.loads(line) for line in stdout.splitlines()
               if line.startswith("{")]
    for reply in replies:
        if "error" in reply:
            raise AssertionError(reply)
        if "model" in reply.get("return", {}):
            return reply["return"]["model"]["props"], stdout + stderr
    raise AssertionError((stdout, stderr))


def check_dump(output, expected):
    for name, value in expected.items():
        line = f"[LATX-SYS] {name} = {int(value)}"
        assert line in output, (line, output)


def main(binary):
    base = "select,aotsys=0,tumode=off,tucpopt=off"
    props, stderr = run_qmp(binary, base + ",verbose=on", "qemu64")
    check_dump(stderr, {"pcid": True, "pcidasid": True,
                        "pcidcache": True, "pcidstats": False})
    props, stderr = run_qmp(
        binary, base + ",verbose=on,pcid=off,pcidasid=off,pcidcache=off,"
        "pcidstats=on", "qemu64")
    check_dump(stderr, {"pcid": False, "pcidasid": False,
                        "pcidcache": False, "pcidstats": True})
    for name, latx, model, props, expected in [
        ("defaults", base, "qemu64", {}, (True, True)),
        ("pcid off", base + ",pcid=off,pcidasid=off,pcidcache=off",
         "qemu64", {}, (False, False)),
        ("explicit off", base, "qemu64", {"pcid": False, "invpcid": False},
         (False, False)),
        ("other model", base, "phenom", {}, (False, False)),
    ]:
        result, _ = run_qmp(binary, latx, model, props)
        actual = result["pcid"], result["invpcid"]
        assert actual == expected, (name, actual, expected)
        print(f"{name}: pcid={actual[0]} invpcid={actual[1]}")


if __name__ == "__main__":
    main(sys.argv[1])
