#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that the AOT writer accepts empty payloads as successful no-ops."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


harness = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static FILE *out_tb_file;
'''
harness += function("static bool aot_save_write(")
harness += r'''
int main(void)
{
    char byte = 0x5a;

    out_tb_file = tmpfile();
    assert(out_tb_file);
    assert(aot_save_write(NULL, 0, 1));
    assert(aot_save_write(NULL, 1, 0));
    assert(ftello(out_tb_file) == 0);
    assert(aot_save_write(&byte, sizeof(byte), 1));
    assert(ftello(out_tb_file) == 1);
    assert(!ferror(out_tb_file));
    assert(fclose(out_tb_file) == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-zero-write-") as directory:
    path = Path(directory)
    cfile, binary = path / "test.c", path / "test"
    cfile.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", str(cfile), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)

print("PASS: empty AOT payloads are successful no-op writes")
