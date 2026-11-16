#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production HAMT SMC counters and inspect refill emission."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ptw-source", type=Path,
                        default=ROOT / "target/i386/hamt/hamt-ptw.c")
    parser.add_argument("--refill-source", type=Path,
                        default=ROOT / "target/i386/latx/optimization/sys-hamt-excp.c")
    args = parser.parse_args()
    source = args.ptw_source.read_text()
    refill = args.refill_source.read_text()
    declarations = source[source.index("uint8_t *ptwdata;"):
                          source.index("static void *hamt_new_ptwdata(")]
    harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "qemu/atomic.h"
#define HAMT_HOLE_ASSERT(p) ((p) != MAP_FAILED)
#define lsassert(p) assert(p)
#define lsassertm(p, ...) assert(p)
""" + declarations
    for signature in ("static void *hamt_new_ptwdata(",
                      "static void *hamt_new_ptwdata1m(",
                      "void hamt_realloc_ptwdata(",
                      "void hamt_ptwdata_smc_protect_page(",
                      "void hamt_ptwdata_smc_unprotect_page("):
        harness += function(source, signature) + "\n"
    harness += r"""
int main(void)
{
    const uint64_t page = 4096, region = 1ULL << 20;
    assert(sizeof(*ptwdata1m) >= 2);
    ptwdata = hamt_new_ptwdata(3 * 256);
    ptwdata1m = hamt_new_ptwdata1m(3);
    assert(ptwdata_size == 3 * 256);
    assert(ptwdata1m_size == 3 * sizeof(*ptwdata1m));
    for (unsigned i = 0; i < 3; i++) {
        assert(ptwdata1m[i] == 0);
    }
    for (unsigned i = 0; i < 255; i++) {
        hamt_ptwdata_smc_protect_page(region + i * page);
        assert(ptwdata1m[1] == i + 1);
    }
    hamt_ptwdata_smc_protect_page(region);
    assert(ptwdata1m[1] == 255);
    hamt_ptwdata_smc_protect_page(region + 255 * page);
    assert(ptwdata1m[1] == 256);
    assert(ptwdata1m[0] == 0 && ptwdata1m[2] == 0);
    hamt_ptwdata_smc_protect_page(2 * region);
    assert(ptwdata1m[1] == 256 && ptwdata1m[2] == 1);
    hamt_ptwdata_smc_unprotect_page(region + 255 * page);
    assert(ptwdata1m[1] == 255 && ptwdata1m[2] == 1);
    hamt_ptwdata_smc_unprotect_page(region + 255 * page);
    assert(ptwdata1m[1] == 255);
    hamt_ptwdata_smc_protect_page(region + 255 * page);
    assert(ptwdata1m[1] == 256);
    for (unsigned i = 0; i < 256; i++) {
        hamt_ptwdata_smc_unprotect_page(region + i * page);
        assert(ptwdata1m[1] == 255 - i);
    }
    hamt_ptwdata_smc_unprotect_page(2 * region);
    assert(ptwdata1m[0] == 0 && ptwdata1m[1] == 0 && ptwdata1m[2] == 0);
    hamt_realloc_ptwdata(4 * region);
    assert(ptwdata_size == 4 * 256);
    assert(ptwdata1m_size == 4 * sizeof(*ptwdata1m));
    for (unsigned i = 0; i < 4; i++) {
        assert(ptwdata1m[i] == 0);
    }
    assert(!munmap(ptwdata, ptwdata_size));
    assert(!munmap(ptwdata1m, ptwdata1m_size));
    puts("PASS: allocation and 0 -> 255 -> 256 -> 255 -> 0 SMC counts");
}
"""
    with tempfile.TemporaryDirectory(prefix="hamt-ptw1m-") as directory:
        binary = Path(directory) / "test"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")),
                        "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "include"), "-x", "c", "-o",
                        str(binary), "-"], input=harness, text=True, check=True)
        subprocess.run([str(binary)], check=True)

    start = refill.index("/* check ptwdata1m[paddr>>20] */")
    end = refill.index("la_out3(LISA_MASKNEZ", start)
    block = refill[start:end]
    patterns = (
        r"la_out2i\(LISA_SRLI_D,\s*&la_t4,\s*&la_t1,\s*19\)",
        r"la_out2i\(LISA_SRLI_D,\s*&la_t6,\s*&la_t2,\s*19\)",
        r"la_out3\s*\(LISA_LDX_HU,\s*&la_t4,\s*&la_a3,\s*&la_t4\)",
        r"la_out3\s*\(LISA_LDX_HU,\s*&la_t6,\s*&la_a3,\s*&la_t6\)",
        r"la_out3\s*\(LISA_OR,\s*&la_a3,\s*&la_t4,\s*&la_t6\)",
        r"la_out2\s*\(LISA_BNEZ,\s*&la_a3,\s*&big_page_do_4k\)",
    )
    position = 0
    for pattern in patterns:
        match = re.search(pattern, block[position:])
        if not match:
            raise AssertionError("missing or reordered refill instruction: " + pattern)
        position += match.end()
    print("PASS: 1M refill emits scaled offsets and unsigned halfword loads")


if __name__ == "__main__":
    main()
