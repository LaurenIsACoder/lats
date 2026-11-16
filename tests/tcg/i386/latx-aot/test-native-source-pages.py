#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check sparse native source dependencies using the actual layout helpers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-format.c").read_text()
begin = source.index("static bool aot_native_source_page_present(")
end = source.index("static bool aot_native_complete_layout_valid_one(", begin)
program = r'''
#include <assert.h>
#include <stdio.h>
#include "lats/aot-format.h"
#include "lats/aot-recover.h"
#define TARGET_PAGE_MASK (~UINT64_C(4095))
''' + source[begin:end] + r'''
int main(void)
{
    struct {
        aot_tu_head head;
        aot_tu_page pages[3];
    } owner = {0};
    struct {
        NativeRecoverHeader header;
        NativeRecoverPoint first;
        NativeRecoverLocation location;
        NativeRecoverPoint second;
    } __attribute__((packed)) table = {0};
    aot_tu_head *tu = &owner.head;
    NativeRecoverHeader *h = &table.header;
    tu->first_pc = 0x400123;
    tu->page_count = 2;
    owner.pages[0].pc_delta = 0;
    owner.pages[1].pc_delta = 0x20000;
    table.first.locations = 1;
    table.second.guest_offset = 0x20000;
    h->points = 2;
    assert(aot_native_recovery_pages_valid(tu, tu->first_pc, 0x20008, h));
    table.second.guest_offset = 0x10000;
    assert(!aot_native_recovery_pages_valid(tu, tu->first_pc, 0x20008, h));
    table.second.guest_offset = 0x20000;
    tu->page_count = 1;
    assert(!aot_native_recovery_pages_valid(tu, tu->first_pc, 0x20008, h));
    tu->page_count = 3;
    owner.pages[2].pc_delta = -0x30000;
    table.second.guest_offset = -0x30000;
    assert(aot_native_recovery_pages_valid(tu, tu->first_pc, 0x20008, h));
    table.second.guest_offset = -0x20000;
    assert(!aot_native_recovery_pages_valid(tu, tu->first_pc, 0x20008, h));
    assert(!aot_native_recovery_pages_valid(tu, tu->first_pc, 0, h));
    assert(!aot_native_recovery_pages_valid(tu, UINT64_MAX - 3, 8, h));
    /* A dependency delta alone must not make a wrapped recovery PC valid. */
    tu->first_pc = 0x1000;
    tu->page_count = 2;
    owner.pages[1].pc_delta = -0x2000;
    table.second.guest_offset = -0x2000;
    assert(!aot_native_recovery_pages_valid(tu, 0x1000, 8, h));
    tu->first_pc = UINT64_MAX - 0xfff;
    owner.pages[1].pc_delta = 0x1000;
    table.second.guest_offset = 0x1000;
    assert(!aot_native_recovery_pages_valid(tu, tu->first_pc, 8, h));
    puts("PASS sparse native source pages");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="lats-native-source-pages-") as temp:
    path = Path(temp)
    (path / "check.c").write_text(program)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-DLATS_AOT_DISK_TU", "-I", str(root / "include"),
                    str(path / "check.c"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
