/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-merge.h"
#include "sys-aot-tu-merge.h"
#include "qemu/rcu.h"

static void aot_free_file_rcu(AOTPublishedFile *file)
{
    munmap(file->tb_ptr, file->size);
    free(file->hash_table);
    free(file);
}

static void aot_defer_free_file(AOTPublishedFile *file)
{
    call_rcu(file, aot_free_file_rcu, rcu);
}

static pthread_t  merger_thread;
static pthread_mutex_t merger_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  merger_cond  = PTHREAD_COND_INITIALIZER;
static volatile uint32_t merge_req;

static volatile int merger_running = 0;

static bool aot_base_file_missing(const char *name)
{
    char *path = g_strdup_printf("%s%s", aot_file_path, name);
    int rc = access(path, F_OK);
    bool missing = rc < 0 && (errno == ENOENT || errno == ENOTDIR);

    g_free(path);
    return missing;
}

/* Keep publication pending after a failed load, even without new increments. */
static bool aot_merge_and_publish(const char *name, uint32_t mode,
                                  AOTPublishedFile **published,
                                  bool *reload_pending, bool refresh)
{
    int merged_count = aot_merge_all(name, mode);
    bool deferred = merged_count == -3;
    AOTPublishedFile *old_file = qatomic_rcu_read(published);

    if ((mode & AOT_USER) && option_aot_save &&
        (deferred || aot_tu_merge_running())) {
        aot_request_tu_merge(deferred);
    }
    if (merged_count == -3) {
        /* Still allow the existing base to be published on first startup. */
        merged_count = 0;
    }
    if (refresh && aot_cross_page_tu_enabled() && (mode & AOT_USER)) {
        *reload_pending = true;
    }
    if (merged_count != 0) {
        /* A failed drain may already have merged some increments. */
        *reload_pending = true;
    }
    if (merged_count < 0) {
        return true;
    }
    if ((!old_file && refresh) || *reload_pending) {
        AOTPublishedFile *file = load_aot_file(name, mode);

        if (!file) {
            /* Missing bases wait for a new save notification. */
            *reload_pending = !aot_base_file_missing(name);
            return *reload_pending;
        }
        qatomic_rcu_set(published, file);
        option_aot_load = 1;
        *reload_pending = false;
        if (old_file) {
            aot_defer_free_file(old_file);
        }
        if (option_verbose) {
            fprintf(stderr, "[AOT][Load] Published: %s (%llu bytes)\n",
                name, (unsigned long long)file->size);
        }
    }
    return deferred || merged_count > 0;
}

static void *merger_thread_func(void *arg) {
    int retry_count = 0;
    bool core_reload_pending = false;
    bool user_reload_pending = false;

    aot_merger_request(AOT_CORE | AOT_USER);
    aot_verbose_log("[AOT] Merger thread started\n");
    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (retry_count > 0) ? 20 : 3600;

        pthread_mutex_lock(&merger_mutex);
        while (merge_req == 0 && merger_running) {
            int rc = pthread_cond_timedwait(&merger_cond, &merger_mutex, &ts);
            if (rc == ETIMEDOUT) {
                if (retry_count > 0) {
                    retry_count--;
                }
                break;
            }
        }

        if (!merger_running) {
            pthread_mutex_unlock(&merger_mutex);
            aot_verbose_log("[AOT] Merger thread stopped\n");
            return NULL;
        }

        bool was_signaled = (merge_req != 0);
        uint32_t pending = was_signaled ? merge_req : (AOT_CORE | AOT_USER);
        qatomic_set(&merge_req, 0);
        pthread_mutex_unlock(&merger_mutex);

        bool need_retry = false;
        if (pending & AOT_CORE) {
            need_retry |= aot_merge_and_publish("sys-aot-core", AOT_CORE,
                    &core_aot_file, &core_reload_pending, was_signaled);
        }
        if (pending & AOT_USER) {
            need_retry |= aot_merge_and_publish("sys-aot-user", AOT_USER,
                    &user_aot_file, &user_reload_pending, was_signaled);
        }

        retry_count = need_retry ? 3 : 0;
    }
}

void aot_merger_start(void) {
    /* Publish before creation so an immediate stop cannot be undone. */
    qatomic_set(&merger_running, 1);
    if (pthread_create(&merger_thread, NULL, merger_thread_func, NULL) != 0) {
        qatomic_set(&merger_running, 0);
        fprintf(stderr, "[AOT] Failed to create merger thread\n");
    }
}

void aot_merger_request(uint32_t req) {
    pthread_mutex_lock(&merger_mutex);
    qatomic_or(&merge_req, req);
    pthread_cond_signal(&merger_cond);
    pthread_mutex_unlock(&merger_mutex);
}

void aot_merger_stop(void) {
    qatomic_set(&merger_running, 0);
    pthread_mutex_lock(&merger_mutex);
    pthread_cond_signal(&merger_cond);
    pthread_mutex_unlock(&merger_mutex);

    if (merger_thread) {
        pthread_join(merger_thread, NULL);
        merger_thread = 0;
    }
    drain_call_rcu();
}

void aot_merger_shutdown_drain(void)
{
    aot_merger_stop();
    aot_merger_drain();
}
