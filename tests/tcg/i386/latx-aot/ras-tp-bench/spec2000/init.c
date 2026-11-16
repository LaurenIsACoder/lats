/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct command { const char *exe, *input, *output, *error; char *const *args; };
struct benchmark { const char *key, *source; const struct command *commands;
    size_t count; const char *const *outputs; size_t output_count; };
#include "manifest.h"

static void fail(const char *what) {
    fprintf(stderr, "SPEC_ERROR %s: %s\n", what, strerror(errno)); exit(1);
}
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock_gettime");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int remove_file(const char *path, const struct stat *s, int type,
                       struct FTW *ftw) {
    (void)s; (void)type; (void)ftw;
    return remove(path);
}
static void copy_tree(const char *source, const char *destination) {
    struct stat s;
    if (stat(source, &s)) fail(source);
    if (S_ISDIR(s.st_mode)) {
        if (mkdir(destination, 0755) && errno != EEXIST) fail(destination);
        DIR *directory = opendir(source);
        if (!directory) fail(source);
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char a[4096], b[4096];
            if ((size_t)snprintf(a, sizeof(a), "%s/%s", source, entry->d_name) >= sizeof(a) ||
                (size_t)snprintf(b, sizeof(b), "%s/%s", destination, entry->d_name) >= sizeof(b))
                fail("path too long");
            copy_tree(a, b);
        }
        closedir(directory);
    } else if (S_ISREG(s.st_mode)) {
        int in = open(source, O_RDONLY), out = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in < 0 || out < 0) fail(source);
        char buffer[16384]; ssize_t length;
        while ((length = read(in, buffer, sizeof(buffer))) > 0) {
            ssize_t done = 0;
            while (done < length) {
                ssize_t n = write(out, buffer + done, length - done);
                if (n <= 0) fail(destination);
                done += n;
            }
        }
        if (length < 0) fail(source);
        close(in); close(out);
    } else fail("unsupported input file");
}
static void redirect(const char *name, int fd, int flags) {
    int file = open(name, flags, 0644);
    if (file < 0 || dup2(file, fd) < 0) fail(name);
    close(file);
}
static void export_tool(const char *exe, char *const argv[], const char *output) {
    pid_t child = fork();
    if (child < 0) fail("export fork");
    if (!child) {
        redirect(output, 1, O_WRONLY | O_CREAT | O_TRUNC);
        execv(exe, argv); fail(exe);
    }
    int status;
    if (waitpid(child, &status, 0) != child) fail("export waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { errno = EIO; fail(exe); }
}
static void export_file(const char *name) {
    struct stat s;
    if (stat(name, &s)) { printf("SPEC_MISSING %s\n", name); return; }
    char *const hash_args[] = {"/bin/busybox", "sha256sum", (char *)name, NULL};
    export_tool(hash_args[0], hash_args, "/tmp/spec-output.sha");
    FILE *hash = fopen("/tmp/spec-output.sha", "r");
    char digest[65];
    if (!hash || fread(digest, 1, 64, hash) != 64) fail("export hash");
    digest[64] = 0; fclose(hash);
    char *const gzip_args[] = {"/bin/busybox", "gzip", "-1", "-c", (char *)name, NULL};
    export_tool(gzip_args[0], gzip_args, "/tmp/spec-output.gz");
    struct stat compressed;
    if (stat("/tmp/spec-output.gz", &compressed)) fail("export gzip");
    FILE *file = fopen("/tmp/spec-output.gz", "rb");
    if (!file) fail("export gzip");
    printf("SPEC_GZIP %s %lld %lld %s\n", name,
        (long long)compressed.st_size, (long long)s.st_size, digest);
    unsigned char buffer[256]; size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file))) {
        static const char digits[] = "0123456789abcdef";
        char encoded[513];
        for (size_t i = 0; i < n; ++i) {
            encoded[2 * i] = digits[buffer[i] >> 4];
            encoded[2 * i + 1] = digits[buffer[i] & 15];
        }
        encoded[2 * n] = 0;
        printf("SPEC_HEX %s\n", encoded);
    }
    if (ferror(file)) fail(name);
    fclose(file); puts("SPEC_FILE_END");
    if (unlink("/tmp/spec-output.gz") || unlink("/tmp/spec-output.sha")) fail("remove export temporary");
}
static void run(size_t index) {
    const struct benchmark *b = &benchmarks[index];
    if (chdir("/")) fail("chdir");
    struct stat s;
    if (!stat("/work", &s) && nftw("/work", remove_file, 16, FTW_DEPTH | FTW_PHYS)) fail("remove work");
    copy_tree(b->source, "/work");
    if (chdir("/work")) fail("work");
    printf("SPEC_BEGIN %s\n", b->key);
    uint64_t elapsed = 0, user_us = 0, system_us = 0;
    int result = 0;
    for (size_t i = 0; i < b->count; ++i) {
        const struct command *c = &b->commands[i];
        uint64_t start = now();
        pid_t child = fork();
        if (child < 0) fail("fork");
        if (!child) {
            redirect(c->input ? c->input : "/dev/null", 0, O_RDONLY);
            redirect(c->output, 1, O_WRONLY | O_CREAT | O_TRUNC);
            redirect(c->error, 2, O_WRONLY | O_CREAT | O_TRUNC);
            execv(c->exe, c->args); fail(c->exe);
        }
        struct rusage usage; int status;
        if (wait4(child, &status, 0, &usage) != child) fail("wait4");
        elapsed += now() - start;
        user_us += (uint64_t)usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec;
        system_us += (uint64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            printf("SPEC_FAILED %zu %d\n", i, status); result = 1; break;
        }
    }
    printf("SPEC_END {\"key\":\"%s\",\"ns\":%llu,\"user_us\":%llu,\"system_us\":%llu,\"failed\":%d}\n",
        b->key, (unsigned long long)elapsed, (unsigned long long)user_us,
        (unsigned long long)system_us, result);
    for (size_t i = 0; i < b->output_count; ++i) export_file(b->outputs[i]);
    for (size_t i = 0; i < b->count; ++i) export_file(b->commands[i].error);
    puts("SPEC_DONE");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mount("proc", "/proc", "proc", 0, NULL)) fail("mount proc");
    if (mount("sysfs", "/sys", "sysfs", 0, NULL)) fail("mount sysfs");
    puts("SPEC_READY");
    char line[128]; size_t index;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "RUN %zu", &index) == 1 && index < benchmark_count) run(index);
        else puts("SPEC_BAD_COMMAND");
    }
    for (;;) pause();
}
