/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IR2_OUTPUT_TEST_SUPPORT_H
#define IR2_OUTPUT_TEST_SUPPORT_H
#include "qemu/osdep.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-options.h"
#include "latx-tu.h"
#include "sys-aot.h"
#include "qemu/log.h"
#include "tcg/tcg-internal.h"
#include "tcg/insn-start-words.h"

int base_tb(const void *);
int new_tb(const void *);
int base_tu(TranslationBlock **);
int new_tu(TranslationBlock **);
int base_search(uint8_t *, uint32_t *);
int new_search(uint8_t *, uint32_t *);
int output_labels_tu(TranslationBlock **);
#endif
