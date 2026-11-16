#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise child completion and publication after a partial AOT save."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#define AOT_CORE 1
#define AOT_USER 2
#define AOT_CHILD_MERGE_FAILED 4
#define AOT_CHILD_EXIT_BASE 64
typedef struct { uint64_t size; } AOTPublishedFile;
static AOTPublishedFile committed = {100};
static int option_verbose, option_aot_load, option_aot_save = 1;
static uint32_t notifications, available;
static int loads;
static bool cross_tu;
static bool aot_cross_page_tu_enabled(void) { return cross_tu; }
static bool aot_tu_merge_running(void) { return false; }
static void aot_request_tu_merge(bool deferred) { (void)deferred; }
static bool aot_base_file_missing(const char *name)
{ return !available; }
#define qatomic_rcu_read(p) (*(p))
#define qatomic_rcu_set(p, v) (*(p) = (v))
static int aot_merge_all(const char *name, uint32_t mode) { return 0; }
static AOTPublishedFile *load_aot_file(const char *name, uint32_t mode)
{ loads++; return (available & mode) ? &committed : NULL; }
static void aot_defer_free_file(AOTPublishedFile *file) { assert(file); }
static void aot_merger_request(uint32_t bits) { notifications |= bits; }
'''
program += function("target/i386/latx/sbt/sys-aot-merger.c",
                    "static bool aot_merge_and_publish(")
program += function("target/i386/latx/sbt/sys-aot-save.c",
                    "static void aot_child_finish(")
program += r'''
int main(void)
{
    for (int code = 0; code < 256; code++) {
        notifications = 0;
        aot_child_finish(42, code << 8);
        uint32_t expected =
            (code & ~(AOT_CORE | AOT_USER | AOT_CHILD_MERGE_FAILED)) ==
            AOT_CHILD_EXIT_BASE ? code & (AOT_CORE | AOT_USER) :
                                 AOT_CORE | AOT_USER;
        assert(notifications == expected);
    }
    const int failures[] = {1 << 8, SIGKILL, SIGSEGV, SIGABRT};
    for (int cp = 0; cp < 2; cp++) {
        cross_tu = cp;
        for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            for (uint32_t mode = AOT_CORE; mode <= AOT_USER; mode++) {
                AOTPublishedFile *published = NULL;
                bool pending = false;
                notifications = 0;
                aot_child_finish(42, failures[i]);
                assert(notifications == (AOT_CORE | AOT_USER));
                /* This base committed before the remaining save failed. */
                available = mode;
                assert(!aot_merge_and_publish("base", mode, &published,
                                              &pending,
                                              notifications & mode));
                assert(published == &committed && !pending);
                /* No committed file: one probe, then no idle load retries. */
                published = NULL;
                available = 0;
                assert(!aot_merge_and_publish("base", mode, &published,
                                              &pending, true));
                int before = loads;
                for (int poll = 0; poll < 10; poll++) {
                    assert(!aot_merge_and_publish("base", mode, &published,
                                                  &pending, false));
                }
                assert(!published && !pending && loads == before);
            }
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="aot-partial-save-") as directory:
    binary = Path(directory) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-x", "c", "-",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-o", str(binary)], input=program, text=True, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if result.returncode:
        print(result.stderr)
    result.check_returncode()
print("PASS: partial child saves publish; absent files wait for notifications")
