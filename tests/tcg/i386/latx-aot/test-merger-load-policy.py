#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check missing-file classification and idle merger retry policy."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
merger = (root / "target/i386/latx/sbt/sys-aot-merger.c").read_text()
start = merger.index("static bool aot_base_file_missing(")
helper = merger[start:merger.index("\n}", start) + 2]
assert "retry_count = need_retry ? 3 : 0;" in merger

program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
static const char *aot_file_path = "/unused/";
static int result, error;
static int test_access(const char *path, int mode)
{
    assert(!strcmp(path, "/unused/sys-aot-user"));
    assert(mode == F_OK);
    errno = error;
    return result;
}
#define access test_access
''' + helper + r'''
int main(void)
{
    result = -1;
    error = ENOENT;
    assert(aot_base_file_missing("sys-aot-user"));
    error = ENOTDIR;
    assert(aot_base_file_missing("sys-aot-user"));
    error = EACCES;
    assert(!aot_base_file_missing("sys-aot-user"));
    error = EIO;
    assert(!aot_base_file_missing("sys-aot-user"));
    result = 0;
    error = ENOENT;
    assert(!aot_base_file_missing("sys-aot-user"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-load-policy-") as directory:
    executable = Path(directory) / "test"
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run([os.environ.get("CC", "cc"), "-x", "c", "-",
                    "-Wall", "-Wextra", "-Werror", "-o", str(executable),
                    *flags], input=program, text=True, check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: missing bases stop retries; transient errors remain retryable")
