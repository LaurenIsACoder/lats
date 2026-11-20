/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dune-info.h"

#define KNRM "\x1B[0m"
#define KRED "\x1B[31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"
#define KBLU "\x1B[34m"
#define KMAG "\x1B[35m"
#define KCYN "\x1B[36m"
#define KWHT "\x1B[37m"

void __attribute__((format(printf, 1, 2)))
dune_warn(const char *err, ...)
{
    va_list params;
    char msg[1024];

    va_start(params, err);
    vsnprintf(msg, sizeof(msg), err, params);
    printf("%s Error: %s%s\n", KCYN, msg, KNRM);
    va_end(params);
}

void __attribute__((format(printf, 1, 2)))
dune_info(const char *info, ...)
{
    va_list params;
    char msg[1024];

    va_start(params, info);
    vsnprintf(msg, sizeof(msg), info, params);
    printf(" Info: %s\n", msg);
    va_end(params);
}

void __attribute__((format(printf, 1, 2)))
dune_die(const char *err, ...)
{
    va_list params;
    char msg[1024];

    va_start(params, err);
    vsnprintf(msg, sizeof(msg), err, params);
    printf("%s Fatal: %s%s\n", KRED, msg, KNRM);
    exit(128);
    va_end(params);
}

void __attribute__((format(printf, 1, 2)))
dune_die_exit(const char *err, ...)
{
    va_list params;
    char msg[1024];

    va_start(params, err);
    vsnprintf(msg, sizeof(msg), err, params);
    printf("%s Fatal: %s%s\n", KRED, msg, KNRM);
    _exit(-1); // child use _exit after fork
    va_end(params);
}
