/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tb-record.h"
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "exec/translation-block.h"

static int tb_record_fd = -1;
static TBRecordEntry *tb_record_buf;
static uint32_t tb_record_buf_count;
static uint64_t tb_record_total;
static QemuMutex tb_record_mutex;
static int tb_record_initialized;

int tb_record_init(const char *output_path)
{
    uint32_t header[4];

    if (tb_record_initialized) {
        return -1;
    }

    tb_record_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tb_record_fd < 0) {
        return -1;
    }

    tb_record_buf = g_malloc(TB_RECORD_BUF_MAX * sizeof(TBRecordEntry));
    if (!tb_record_buf) {
        close(tb_record_fd);
        tb_record_fd = -1;
        return -1;
    }

    tb_record_buf_count = 0;
    tb_record_total = 0;
    qemu_mutex_init(&tb_record_mutex);

    header[0] = TB_RECORD_MAGIC;
    header[1] = TB_RECORD_VERSION;
    header[2] = 0;
    header[3] = 0;
    if (write(tb_record_fd, header, sizeof(header)) != sizeof(header)) {
        close(tb_record_fd);
        tb_record_fd = -1;
        g_free(tb_record_buf);
        tb_record_buf = NULL;
        return -1;
    }

    tb_record_initialized = 1;
    return 0;
}

static int write_record_to_fd(int fd, const TBRecordEntry *entry)
{
    ssize_t written;

    written = write(fd, &entry->hdr, sizeof(entry->hdr));
    if (written != sizeof(entry->hdr)) {
        return -1;
    }

    if (entry->hdr.ir1_len > 0) {
        written = write(fd, entry->ir1_data, entry->hdr.ir1_len);
        if (written != (ssize_t)entry->hdr.ir1_len) {
            return -1;
        }
    }

    if (entry->hdr.ir2_len > 0) {
        written = write(fd, entry->ir2_data, entry->hdr.ir2_len);
        if (written != (ssize_t)entry->hdr.ir2_len) {
            return -1;
        }
    }

    return 0;
}

int tb_record_flush(void)
{
    uint32_t i;

    qemu_mutex_lock(&tb_record_mutex);

    if (tb_record_buf_count == 0 || tb_record_fd < 0) {
        qemu_mutex_unlock(&tb_record_mutex);
        return 0;
    }

    for (i = 0; i < tb_record_buf_count; i++) {
        if (write_record_to_fd(tb_record_fd, &tb_record_buf[i]) < 0) {
            qemu_mutex_unlock(&tb_record_mutex);
            return -1;
        }
        g_free((void *)tb_record_buf[i].ir1_data);
        g_free((void *)tb_record_buf[i].ir2_data);
    }

    tb_record_total += tb_record_buf_count;
    tb_record_buf_count = 0;

    qemu_mutex_unlock(&tb_record_mutex);
    return 0;
}

int tb_record_tb(void *tb_ptr,
                 const uint8_t *ir1_data, uint32_t ir1_len,
                 const uint32_t *ir2_data, uint32_t ir2_len)
{
    TranslationBlock *tb = (TranslationBlock *)tb_ptr;
    TBRecordEntry *entry;

    if (!tb_record_initialized || tb_record_fd < 0) {
        return -1;
    }

    qemu_mutex_lock(&tb_record_mutex);

    if (tb_record_buf_count >= TB_RECORD_BUF_MAX) {
        uint32_t i;
        for (i = 0; i < tb_record_buf_count; i++) {
            if (write_record_to_fd(tb_record_fd, &tb_record_buf[i]) < 0) {
                qemu_mutex_unlock(&tb_record_mutex);
                return -1;
            }
            /* Free allocated ir1/ir2 data */
            g_free((void *)tb_record_buf[i].ir1_data);
            g_free((void *)tb_record_buf[i].ir2_data);
        }
        tb_record_total += tb_record_buf_count;
        tb_record_buf_count = 0;
    }

    entry = &tb_record_buf[tb_record_buf_count];
    entry->hdr.pc = tb->pc;
    entry->hdr.cs_base = (uint32_t)tb->cs_base;
    entry->hdr.flags = tb->flags;
    entry->hdr.cflags = tb->cflags;
    entry->hdr.page_addr0 = (uint64_t)tb->page_addr[0];
    entry->hdr.page_addr1 = (uint64_t)tb->page_addr[1];
    entry->hdr.size = tb->size;
    entry->hdr.icount = tb->icount;
    entry->hdr.ir1_len = ir1_len;
    entry->hdr.ir2_len = ir2_len;

    /* Allocate and copy ir1 data (original buffer is thread-local and gets reused) */
    if (ir1_len > 0 && ir1_data) {
        entry->ir1_data = g_malloc(ir1_len);
        if (!entry->ir1_data) {
            qemu_mutex_unlock(&tb_record_mutex);
            return -1;
        }
        memcpy((void *)entry->ir1_data, ir1_data, ir1_len);
    } else {
        entry->ir1_data = NULL;
    }

    /* Allocate and copy ir2 data */
    if (ir2_len > 0 && ir2_data) {
        entry->ir2_data = g_malloc(ir2_len);
        if (!entry->ir2_data) {
            g_free((void *)entry->ir1_data);
            entry->ir1_data = NULL;
            qemu_mutex_unlock(&tb_record_mutex);
            return -1;
        }
        memcpy((void *)entry->ir2_data, ir2_data, ir2_len);
    } else {
        entry->ir2_data = NULL;
    }

    tb_record_buf_count++;

    qemu_mutex_unlock(&tb_record_mutex);
    return 0;
}

int tb_record_fini(void)
{
    uint32_t header[4];
    int ret = 0;

    if (!tb_record_initialized) {
        return 0;
    }

    qemu_mutex_lock(&tb_record_mutex);

    if (tb_record_buf_count > 0 && tb_record_fd >= 0) {
        uint32_t i;
        for (i = 0; i < tb_record_buf_count; i++) {
            if (write_record_to_fd(tb_record_fd, &tb_record_buf[i]) < 0) {
                ret = -1;
            }
            g_free((void *)tb_record_buf[i].ir1_data);
            g_free((void *)tb_record_buf[i].ir2_data);
        }
        tb_record_total += tb_record_buf_count;
        tb_record_buf_count = 0;
    }

    if (tb_record_fd >= 0) {
        header[0] = TB_RECORD_MAGIC;
        header[1] = TB_RECORD_VERSION;
        header[2] = (uint32_t)tb_record_total;
        header[3] = 0;
        if (lseek(tb_record_fd, 0, SEEK_SET) == (off_t)-1 ||
                write(tb_record_fd, header, sizeof(header)) != sizeof(header)) {
            ret = -1;
        }
        close(tb_record_fd);
        tb_record_fd = -1;
    }

    qemu_mutex_unlock(&tb_record_mutex);
    qemu_mutex_destroy(&tb_record_mutex);

    g_free(tb_record_buf);
    tb_record_buf = NULL;
    tb_record_initialized = 0;

    return ret;
}

uint64_t tb_record_count(void)
{
    uint64_t count;
    qemu_mutex_lock(&tb_record_mutex);
    count = tb_record_total + tb_record_buf_count;
    qemu_mutex_unlock(&tb_record_mutex);
    return count;
}
