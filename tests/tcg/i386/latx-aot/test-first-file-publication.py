#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the production merger publication decision with injected I/O."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
text = (root / "target/i386/latx/sbt/sys-aot-merger.c").read_text()
start = text.index("static bool aot_merge_and_publish(")
function = text[start:text.index("\n}", start) + 2]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { uint64_t size; } AOTPublishedFile;
static AOTPublishedFile first = {100}, second = {200};
static AOTPublishedFile *available;
static int merge_result, loads, retired, option_aot_load;
static int option_verbose, option_aot_save = 1;
#define AOT_USER 2
static bool cross_tu, worker_running, missing;
static int requests;
static bool aot_cross_page_tu_enabled(void) { return cross_tu; }
static bool aot_tu_merge_running(void) { return worker_running; }
static void aot_request_tu_merge(bool merge) { (void)merge; requests++; }
static bool aot_base_file_missing(const char *name) { return missing; }
#define qatomic_rcu_read(p) (*(p))
#define qatomic_rcu_set(p, v) (*(p) = (v))
static int aot_merge_all(const char *n, uint32_t m) { return merge_result; }
static AOTPublishedFile *load_aot_file(const char *n, uint32_t m)
{ loads++; return available; }
static void aot_defer_free_file(AOTPublishedFile *f) { assert(f); retired++; }
''' + function + r'''
int main(void) {
    for (uint32_t mode = 1; mode <= 2; mode++) {
        AOTPublishedFile *published = 0;
        bool pending = false;
        loads = retired = option_aot_load = merge_result = 0;
        available = 0;
        missing = true;
        /* Missing base is harmless and can be retried by a later request. */
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      true));
        assert(!published && !pending && loads == 1);
        for (int i = 0; i < 10; i++) {
            assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                          false));
        }
        assert(loads == 1);
        available = &first;
        missing = false;
        /* Only a notification, not an idle poll, loads a new base. */
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(!published && loads == 1);
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      true));
        assert(published == &first && option_aot_load && loads == 2);
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(loads == 2 && !retired);
        /* A failed load after a successful merge retains the old file. */
        merge_result = 1; available = 0;
        assert(aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(pending && published == &first && !retired);
        merge_result = 0; available = &second;
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(!pending && published == &second && retired == 1);
        /* Partial merge failure must still cause a later reload. */
        merge_result = -1;
        int old_loads = loads;
        assert(aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(pending && loads == old_loads);
        merge_result = 0; available = &first;
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(published == &first && !pending && retired == 2);
        /* A first-load lock failure remains pending without another signal. */
        published = 0; available = 0;
        assert(aot_merge_and_publish("base", mode, &published, &pending,
                                     true));
        assert(pending);
        available = &second;
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(published == &second && !pending);
        /* A missing base stops retries without dropping published data. */
        merge_result = 1; available = 0; missing = true;
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(published == &second && !pending);
        merge_result = 0;
        old_loads = loads;
        assert(!aot_merge_and_publish("base", mode, &published, &pending,
                                      false));
        assert(loads == old_loads);
    }
    AOTPublishedFile *published = &first;
    bool pending = false;
    cross_tu = true; merge_result = -3; available = &first;
    int before = requests;
    assert(aot_merge_and_publish("base", AOT_USER, &published, &pending,
                                      false));
    assert(requests == before + 1 && published == &first);
    merge_result = 0; available = &second;
    assert(!aot_merge_and_publish("base", AOT_USER, &published, &pending,
                                      true));
    assert(published == &second && !pending);
    worker_running = true; before = requests;
    assert(!aot_merge_and_publish("base", AOT_USER, &published, &pending,
                                      false));
    assert(requests == before + 1);
}
'''
with tempfile.TemporaryDirectory(prefix="aot-publish-") as work:
    binary = str(Path(work) / "test")
    subprocess.run([os.environ.get("CC", "cc"), "-x", "c", "-",
                    "-o", binary], input=source, text=True, check=True)
    subprocess.run([binary], check=True)
print("PASS: notified publication, idle polls and transient load retries")
