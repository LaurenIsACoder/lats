#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production spool creation when the system tmpfile hits its quota."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-common.c").read_text()
start = source.index("FILE *aot_open_spool_file(void)")
function = source[start:source.index("\n}", start) + 2]

prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char *aot_file_path;
static int block_tmpfile, fail_fdopen, failed_fd = -1, tmpfile_calls;
static FILE *quota_tmpfile(void)
{
    tmpfile_calls++;
    if (block_tmpfile) { errno = EDQUOT; return NULL; }
    return tmpfile();
}
static FILE *fixture_fdopen(int fd, const char *mode)
{
    if (fail_fdopen) { failed_fd = fd; errno = EMFILE; return NULL; }
    return fdopen(fd, mode);
}
#define tmpfile quota_tmpfile
#define fdopen fixture_fdopen
'''
main = r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    aot_file_path = argv[1];
    block_tmpfile = 1;
    FILE *file = aot_open_spool_file();
    /* The original tmpfile() path fails here with EDQUOT. */
    if (!file) { return 1; }
    assert(tmpfile_calls == 0);
    struct stat cache, spool;
    assert(stat(aot_file_path, &cache) == 0);
    assert(fstat(fileno(file), &spool) == 0);
    assert(cache.st_dev == spool.st_dev && spool.st_nlink == 0);
    assert((fcntl(fileno(file), F_GETFL) & O_ACCMODE) == O_RDWR);
    assert(fputs("cross-page TU", file) >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    char bytes[32];
    assert(fgets(bytes, sizeof(bytes), file));
    assert(strcmp(bytes, "cross-page TU") == 0);
    assert(fclose(file) == 0);

    /* A failed stream conversion releases the unlinked descriptor. */
    fail_fdopen = 1;
    assert(aot_open_spool_file() == NULL);
    assert(failed_fd >= 0);
    assert(fcntl(failed_fd, F_GETFD) == -1 && errno == EBADF);
    fail_fdopen = 0;
    block_tmpfile = 0;

    /* Keep the existing tmpfile fallback when the cache is unavailable. */
    const char *paths[] = { NULL, "", "/jitbench-no-such-directory/cache/" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        aot_file_path = (char *)paths[i];
        int before = tmpfile_calls;
        file = aot_open_spool_file();
        assert(file && tmpfile_calls == before + 1);
        assert(fclose(file) == 0);
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-spool-test-") as directory:
    path = Path(directory)
    cache = path / "cache"
    cache.mkdir()
    for name, body in [
        ("production", function),
        ("old-path", "FILE *aot_open_spool_file(void) { return tmpfile(); }"),
    ]:
        cfile, binary = path / (name + ".c"), path / name
        cfile.write_text(prefix + body + main)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-function",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary), str(cache) + "/"])
        assert result.returncode == (0 if name == "production" else 1)
        assert list(cache.iterdir()) == []

print("PASS: cache-backed spool survives EDQUOT; fallback and fd cleanup verified")
