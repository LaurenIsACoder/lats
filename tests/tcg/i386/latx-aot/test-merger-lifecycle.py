#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that a delayed merger start cannot undo an immediate stop."""

from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-merger.c").read_text()


def function(name):
    start = source.index("void " + name + "(")
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


worker = source[source.index("static void *merger_thread_func("):
                source.index("void aot_merger_start(")]
assert "qatomic_set(&merger_running, 1)" not in worker
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
static int merger_running, merger_mutex, merger_cond;
static uintptr_t merger_thread;
static int fail_create, joins, drains;
#define qatomic_set(p, v) (*(p) = (v))
static void *merger_thread_func(void *arg) { return arg; }
static int pthread_create(uintptr_t *thread, void *attr,
                          void *(*run)(void *), void *arg)
{
    assert(merger_running);
    if (fail_create) return 1;
    *thread = 42;
    return 0;
}
static int pthread_mutex_lock(int *lock) { return 0; }
static int pthread_mutex_unlock(int *lock) { return 0; }
static int pthread_cond_signal(int *cond) { return 0; }
static int pthread_join(uintptr_t thread, void **result)
{
    assert(thread == 42 && !merger_running);
    joins++;
    return 0;
}
static void drain_call_rcu(void) { drains++; }
''' + function("aot_merger_start") + function("aot_merger_stop") + r'''
int main(void)
{
    aot_merger_start();
    aot_merger_stop();
    assert(!merger_running && !merger_thread && joins == 1);
    aot_merger_stop();
    assert(joins == 1 && drains == 2);
    fail_create = 1;
    aot_merger_start();
    assert(!merger_running && !merger_thread);
    aot_merger_stop();
    assert(joins == 1);
}
'''
with tempfile.TemporaryDirectory(prefix="aot-merger-lifecycle-") as directory:
    executable = Path(directory) / "test"
    subprocess.run(["cc", "-x", "c", "-Wall", "-Werror", "-o",
                    str(executable), "-"], input=program, text=True, check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: merger immediate stop, repeated stop and creation failure")
