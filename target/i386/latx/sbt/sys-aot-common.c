/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-common.h"
#include "sys-aot-format.h"
#include "file_ctx.h"
#include "qemu/cutils.h"
#ifdef CONFIG_SOFTMMU
#include "exec/cpu-common.h"
#include "exec/ramblock.h"
#include "hw/boards.h"
#endif

#ifdef CONFIG_SOFTMMU
bool aot_host_pc_in_main_ram(void *host_pc)
{
    ram_addr_t offset;
    RAMBlock *block = qemu_ram_block_from_host(host_pc, false, &offset);

    return block && block->mr == current_machine->ram;
}
#endif

char *gen_file_lock(const char *file_name)
{
    size_t len = strlen(file_name);
    char *aot_file_lock = malloc(len + 10);
    if (!aot_file_lock) {
        fprintf(stderr, "[AOT] File: malloc lock name failed\n");
        return NULL;
    }
    snprintf(aot_file_lock, len + 10, "%s.lock", file_name);
    return aot_file_lock;
}

static int file_exists(const char *filename)
{
    return access(filename, F_OK) == 0;
}

static void aot_make_tmp_name(char *tmp_name, int size, const char *base)
{
    pstrcpy(tmp_name, size, base);
    pstrcat(tmp_name, size, ".tmp");
}

void aot_generate_new_filename(const char *file_path, const char *filename,
        char *new_name, size_t size)
{
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s%s", file_path, filename);

    char tmp_name[PATH_MAX];
    aot_make_tmp_name(tmp_name, sizeof(tmp_name), base);
    if (file_exists(tmp_name)) {
        remove(tmp_name);
    }

    if (!file_exists(base)) {
        strncpy(new_name, base, size);
        new_name[size - 1] = '\0';
        return;
    }

    int i = 1;
    while (1) {
        snprintf(new_name, size, "%s-add%d", base, i);
        if (!file_exists(new_name)) {
            break;
        }
        i++;
    }
}

void aot_file_writer_init(AOTFileWriter *writer, const char *file_path,
        const char *file_name)
{
    memset(writer, 0, sizeof(*writer));
    writer->lockfd = -1;
    snprintf(writer->final_name, sizeof(writer->final_name), "%s%s",
            file_path, file_name);
}

static void aot_file_writer_set_candidate(AOTFileWriter *writer,
        const char *base_name, int index)
{
    pstrcpy(writer->final_name, sizeof(writer->final_name), base_name);
    if (index > 0) {
        char suffix[32];

        snprintf(suffix, sizeof(suffix), "-add%d", index);
        pstrcat(writer->final_name, sizeof(writer->final_name), suffix);
    }
    aot_make_tmp_name(writer->tmp_name, sizeof(writer->tmp_name),
            writer->final_name);
}

static void aot_file_writer_unlock_candidate(AOTFileWriter *writer)
{
    if (writer->lockfd >= 0) {
        flock_set(writer->lockfd, F_UNLCK, true);
        close(writer->lockfd);
        writer->lockfd = -1;
    }
    free(writer->lock_name);
    writer->lock_name = NULL;
}

FILE *aot_file_writer_open(AOTFileWriter *writer)
{
    char base_name[PATH_MAX];

    pstrcpy(base_name, sizeof(base_name), writer->final_name);

    for (int i = 0; ; i++) {
        aot_file_writer_set_candidate(writer, base_name, i);
        writer->lock_name = gen_file_lock(writer->final_name);
        if (!writer->lock_name) {
            return NULL;
        }
        writer->lockfd = -1;
        if (file_lock(writer->lock_name, &writer->lockfd, F_WRLCK, true) < 0) {
            fprintf(stderr, "[AOT] File: lock failed\n");
            aot_file_writer_unlock_candidate(writer);
            return NULL;
        }

        if (!file_exists(writer->final_name)) {
            break;
        }

        aot_file_writer_unlock_candidate(writer);
    }

    if (file_exists(writer->tmp_name)) {
        remove(writer->tmp_name);
    }

    writer->file = fopen(writer->tmp_name, "wb+");
    if (!writer->file) {
        fprintf(stderr, "[AOT] File: open output %s failed, errno=%d, err_msg=\"%s\", line:%d\n",
                writer->tmp_name, errno, strerror(errno), __LINE__);
        aot_file_writer_unlock_candidate(writer);
        return NULL;
    }

    out_tb_file = writer->file;
    return writer->file;
}

void aot_file_writer_abort(AOTFileWriter *writer)
{
    bool remove_tmp = writer->file != NULL;

    if (writer->file) {
        fclose(writer->file);
        writer->file = NULL;
        out_tb_file = NULL;
    }
    if (remove_tmp && writer->tmp_name[0]) {
        remove(writer->tmp_name);
    }
    aot_file_writer_unlock_candidate(writer);
}

bool aot_file_writer_commit(AOTFileWriter *writer)
{
    bool success = true;

    if (fflush(writer->file) == EOF || fsync(fileno(writer->file)) < 0) {
        success = false;
    }
    if (fclose(writer->file) == EOF) {
        success = false;
    }
    writer->file = NULL;
    out_tb_file = NULL;

    if (success && rename(writer->tmp_name, writer->final_name) < 0) {
        fprintf(stderr, "[AOT] File: rename %s -> %s failed\n", writer->tmp_name,
                writer->final_name);
        success = false;
    }
    if (!success) {
        fprintf(stderr, "[AOT] File: commit %s failed: %s\n",
                writer->final_name, strerror(errno));
        remove(writer->tmp_name);
    }

    flock_set(writer->lockfd, F_UNLCK, true);
    close(writer->lockfd);
    writer->lockfd = -1;
    free(writer->lock_name);
    writer->lock_name = NULL;
    return success;
}

void *load_file_to_mem(char *file_name, uint64_t *size, uint32_t aot_mode)
{
    struct stat sb;
    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "[AOT] File: open %s failed\n", file_name);
        return NULL;
    }
    if (fstat(fd, &sb) == -1) {
        fprintf(stderr, "[AOT] File: fstat %s failed\n", file_name);
        close(fd);
        return NULL;
    }
    if (option_verbose) {
        fprintf(stderr, "[AOT][File] %s size=%ld MB\n",
                file_name, sb.st_size / 1000000);
    }
    if (sb.st_size < sizeof(aot_tail)) {
        fprintf(stderr, "[AOT] File: truncated file: %s\n", file_name);
        remove(file_name);
        close(fd);
        return NULL;
    }
    void *mem_ptr = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE, fd, 0);
    if (mem_ptr == MAP_FAILED) {
        fprintf(stderr, "[AOT] File: mmap failed, errno=%d, err_msg=\"%s\", line:%d\n",
                errno, strerror(errno), __LINE__);
        close(fd);
        return NULL;
    }
    if (!validate_aot_layout(mem_ptr, sb.st_size)) {
        fprintf(stderr, "[AOT] File: invalid layout: %s\n", file_name);
        munmap(mem_ptr, sb.st_size);
        remove(file_name);
        close(fd);
        return NULL;
    }
    if (!test_integrity(fd, aot_mode)) {
        fprintf(stderr, "[AOT] File: integrity check failed: %s\n", file_name);
        munmap(mem_ptr, sb.st_size);
        remove(file_name);
        close(fd);
        return NULL;
    }
    *size = sb.st_size;
    close(fd);
    return mem_ptr;
}
