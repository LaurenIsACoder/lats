#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Test production SQE helpers with liburing stubs, not a kernel ring.

FDMON_IOURING_TEST_SOURCE selects an older source for regression checks.
Run --case lifetime --asan to detect timeout storage used after return.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=('tags', 'lifetime'), default='tags')
parser.add_argument('--asan', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get(
    'FDMON_IOURING_TEST_SOURCE', root / 'util/fdmon-io_uring.c')).read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#define likely(v) (v)
#define unlikely(v) (v)
#define NANOSECONDS_PER_SECOND INT64_C(1000000000)
#define LIBURING_HAVE_DATA64 1
#define G_IO_IN POLLIN
#define G_IO_OUT POLLOUT
#define G_IO_HUP POLLHUP
#define G_IO_ERR POLLERR
#define FDMON_IO_URING_REMOVE 4
#define qatomic_fetch_and(p, v) \
    __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define QLIST_INSERT_HEAD_RCU(h, n, f) \
    do { (void)(h); (void)(n); deleted++; } while (0)
typedef uint64_t __u64;
struct __kernel_timespec {
    int64_t tv_sec, tv_nsec;
};
struct io_uring_sqe {
    uint64_t user_data;
    const struct __kernel_timespec *ts;
};
struct io_uring_cqe {
    uint64_t user_data;
    int res;
};
struct io_uring {
    struct io_uring_sqe sqe;
    bool full;
};
typedef struct AioHandler {
    unsigned flags;
    struct { int fd, events; } pfd;
} AioHandler;
typedef struct {
    struct io_uring fdmon_io_uring;
    int deleted_aio_handlers;
} AioContext;
typedef struct { int unused; } AioHandlerList;
static unsigned dispatched, deleted, submissions;
static bool check_timeout;
static uint64_t cancellation_target;

static struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
    return ring->full ? NULL : &ring->sqe;
}

static int io_uring_submit(struct io_uring *ring)
{
    submissions++;
    ring->full = false;
    return 2;
}

static void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data)
{
    sqe->user_data = (uintptr_t)data;
}

static void *io_uring_cqe_get_data(struct io_uring_cqe *cqe)
{
    return (void *)(uintptr_t)cqe->user_data;
}

/* Match liburing: preparation preserves user_data and stores the pointer. */
static void io_uring_prep_poll_add(struct io_uring_sqe *sqe, int fd, int events)
{
    (void)sqe;
    (void)fd;
    (void)events;
}

static void io_uring_prep_poll_remove(struct io_uring_sqe *sqe, uint64_t data)
{
    (void)sqe;
    cancellation_target = data;
}

static void io_uring_prep_timeout(struct io_uring_sqe *sqe,
                                const struct __kernel_timespec *ts,
                                unsigned count, unsigned flags)
{
    assert(count == 1 && flags == 0);
    sqe->ts = ts;
}

static void aio_add_ready_handler(AioHandlerList *list, AioHandler *handler,
                                  int events)
{
    (void)list;
    (void)handler;
    (void)events;
    dispatched++;
}

static void fill_sq_ring(AioContext *ctx)
{
    (void)ctx;
}

static __attribute__((noinline))
int io_uring_submit_and_wait(struct io_uring *ring, unsigned count)
{
    assert(count == 1);
    submissions++;
    if (check_timeout) {
        /* Consume the timeout only at submission, after preparation returns. */
        assert(ring->sqe.ts->tv_sec == 2);
        assert(ring->sqe.ts->tv_nsec == 123456789);
    }
    return 1;
}
'''
signatures = [
    'static inline int poll_events_from_pfd(',
    'static inline int pfd_events_from_poll(',
    'static struct io_uring_sqe *get_sqe(',
    'static void add_poll_add_sqe(',
    'static void add_poll_remove_sqe(',
]
# The upstream lifetime fix inlines this helper. Include it for the old
# source so the same test can demonstrate the regression before the fix.
if 'static void add_timeout_sqe(' in source:
    signatures.append('static void add_timeout_sqe(')
signatures.append('static bool process_cqe(')
for signature in signatures:
    program += function(signature) + '\n'
program += r'''
static int process_cq_ring(AioContext *ctx, AioHandlerList *list)
{
    struct io_uring_cqe cqe = {
        .user_data = ctx->fdmon_io_uring.sqe.user_data,
        .res = -ETIME,
    };

    return process_cqe(ctx, list, &cqe);
}
'''
program += function('static int fdmon_io_uring_wait(') + '\n'
program += r'''
int main(int argc, char **argv)
{
    AioContext ctx = {0};
    AioHandlerList ready = {0};
    AioHandler old = {.pfd = {3, POLLIN}};
    AioHandler removed = {.pfd = {4, POLLIN}};
    struct io_uring_cqe cqe;
    int full;

    if (argc > 1 && !strcmp(argv[1], "lifetime")) {
        check_timeout = true;
        assert(fdmon_io_uring_wait(&ctx, &ready, INT64_C(2123456789)) == 0);
        return 0;
    }
    for (full = 0; full < 2; full++) {
        ctx.fdmon_io_uring.full = full;
        ctx.fdmon_io_uring.sqe.user_data = (uintptr_t)&old;
        assert(fdmon_io_uring_wait(&ctx, &ready, INT64_C(2123456789)) == 0);
        assert(dispatched == 0 && deleted == 0);

        ctx.fdmon_io_uring.full = full;
        ctx.fdmon_io_uring.sqe.user_data = (uintptr_t)&old;
        add_poll_remove_sqe(&ctx, &removed);
        assert(cancellation_target == (uintptr_t)&removed);
        cqe = (struct io_uring_cqe){
            .user_data = ctx.fdmon_io_uring.sqe.user_data,
            .res = 0,
        };
        assert(!process_cqe(&ctx, &ready, &cqe));
        assert(dispatched == 0 && deleted == 0);
    }
    /* A real poll completion still dispatches and re-arms its handler. */
    add_poll_add_sqe(&ctx, &old);
    cqe = (struct io_uring_cqe){
        .user_data = ctx.fdmon_io_uring.sqe.user_data,
        .res = POLLIN,
    };
    assert(process_cqe(&ctx, &ready, &cqe));
    assert(dispatched == 1 && deleted == 0);
    assert(ctx.fdmon_io_uring.sqe.user_data == (uintptr_t)&old);
    assert(submissions >= 4);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='fdmon-sqe-') as directory:
    binary = Path(directory) / 'test'
    flags = ['-std=gnu11', '-O0', '-fno-inline', '-g',
             '-Wall', '-Wextra', '-Werror']
    if args.asan:
        flags += ['-fsanitize=address', '-fsanitize-address-use-after-scope']
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-x', 'c'] + flags + ['-o', str(binary), '-'],
                   input=program, text=True, check=True, timeout=30)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_stack_use_after_return=1:detect_leaks=0'
    subprocess.run([str(binary), args.case], env=env, check=True, timeout=20)
print('PASS:', args.case, 'ASan' if args.asan else '')
