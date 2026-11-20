/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-tu-codec.h"
#include "latx-misc.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "accel/tcg/internal.h"

static bool aot_codec_write(FILE *file, const void *ptr, size_t size, size_t n)
{
    return !size || !n || fwrite(ptr, size, n, file) == n;
}

void aot_encode_block(aot_block *aot_tb, TranslationBlock *tb)
{
    /* memcpy(aot_tb, tb, sizeof(TranslationBlock)); */
    aot_tb->pc = tb->pc;
    aot_tb->jmp_reset_offset[0] = tb->jmp_reset_offset[0];
    aot_tb->jmp_reset_offset[1] = tb->jmp_reset_offset[1];
    aot_tb->jmp_insn_offset[0] = tb->jmp_insn_offset[0];
    aot_tb->jmp_insn_offset[1] = tb->jmp_insn_offset[1];
    aot_tb->jmp_target_addr[0] = tb->jmp_target_addr[0];
    aot_tb->jmp_target_addr[1] = tb->jmp_target_addr[1];
    aot_tb->inst_ptr_offset = tb->inst_ptr_offset;
    aot_tb->label_pos_nocheck = tb->label_pos_nocheck;
    aot_tb->link_opt[0] = tb->link_opt[0];
    aot_tb->link_opt[1] = tb->link_opt[1];
    aot_tb->int_opt[0] = tb->int_opt[0];
    aot_tb->int_opt[1] = tb->int_opt[1];
    aot_tb->eflag_use = tb->s_data->eflag_use;
    aot_tb->eflag_use_real = tb->s_data->eflag_use_real;
    aot_tb->last_ir1_type = tb->s_data->last_ir1_type;
    aot_tb->tu_tb_mode = tb->s_data->tu_tb_mode;
    aot_tb->next_pc[0] = tb->next_pc[0];
    aot_tb->next_pc[1] = tb->next_pc[1];
    aot_tb->jmp_cp_data[0] = tb->jmp_cp_data[0];
    aot_tb->jmp_cp_data[1] = tb->jmp_cp_data[1];
    aot_tb->tu_generation_offset_patch[0] =
        tb->s_data->tu_generation_offset_patch[0];
    aot_tb->tu_generation_offset_patch[1] =
        tb->s_data->tu_generation_offset_patch[1];
    aot_tb->misc_flags = tb->misc_flags;
    aot_tb->flags = tb->flags;
    aot_tb->cflags = tb->cflags;
    aot_tb->size = tb->size;
    aot_tb->icount = tb->icount;
    aot_tb->ir2_num = tb->ir2_num;
    /* Do not serialize the search_ptr arm of the union as a byte count. */
    aot_tb->search_size = option_split_search ? tb->icount * sizeof(int) :
                                               tb->search_size;
    aot_tb->x86_next_pc = tb->s_data->x86_next_pc;
    aot_tb->x86_target_pc = tb->s_data->x86_target_pc;
    aot_tb->tu_jmp[0] = tb->s_data->tu_jmp[0];
    aot_tb->tu_jmp[1] = tb->s_data->tu_jmp[1];
    aot_tb->tc.offset_in_tu = tb->tc.offset_in_tu;
    aot_tb->rel_start = TB_REL_START(tb);
    aot_tb->rel_end = TB_REL_END(tb);
#ifdef CONFIG_LATX_TU
    aot_tb->tu_size = tb->s_data->tu_size;
#endif
    aot_tb->cs_base = tb->cs_base;
    aot_tb->page_addr[0] = tb->page_addr[0];
    aot_tb->page_addr[1] = tb->page_addr[1];
#ifdef CONFIG_LATX_TU
    aot_tb->ptn.eflag_type = tb->ptn.eflag_type;
    if (tb->ptn.eflag_type != PTN_EFLAG_TYPE_INVALID) {
        aot_tb->ptn.os = tb->ptn.os;
        aot_tb->ptn.opnd0 = tb->ptn.opnd0;
        aot_tb->ptn.opnd1 = tb->ptn.opnd1;
        aot_tb->ptn.pcoffset = tb->ptn.pcoffset;
    }
#endif
}

#ifdef CONFIG_LATX_TU
static uint64_t aot_tu_id_mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t aot_make_tu_id(TranslationBlock **tb_vector, int tb_count,
        const aot_tu_page *pages, uint32_t page_count)
{
    target_ulong anchor_page = tb_vector[0]->pc & TARGET_PAGE_MASK;
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = aot_tu_id_mix(hash, tb_vector[0]->pc & ~TARGET_PAGE_MASK);
    hash = aot_tu_id_mix(hash, page_count);
    for (uint32_t i = 0; i < page_count; i++) {
        hash = aot_tu_id_mix(hash, pages[i].pc_delta);
        hash = aot_tu_id_mix(hash, pages[i].page_fingerprint);
    }
    hash = aot_tu_id_mix(hash, tb_count);
    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        hash = aot_tu_id_mix(hash, tb->pc - anchor_page);
        hash = aot_tu_id_mix(hash, tb->cs_base);
        hash = aot_tu_id_mix(hash, tb->flags);
        hash = aot_tu_id_mix(hash, tb->cflags);
    }
    return hash ? hash : 1;
}

bool aot_encode_tu(FILE *file, TranslationBlock **tb_vector, int tb_count,
                   const aot_tu_page *tu_pages, uint32_t mapping_count,
                   int *record_size, uint64_t *tu_id)
{
    aot_tu_head tu_head = { 0 };
    aot_rel *saved_rels = NULL;
    int curr_ir2_size = 0;
    int saved_rel_count = 0;
    bool ok = false;

    if (!file || !mapping_count || mapping_count > AOT_TU_MAX_PAGES ||
            tb_count <= 0 || !tb_vector[0]->s_data->is_first_tb) {
        return false;
    }

    tu_head.tb_magic = AOT_TB_MAGIC;
    tu_head.tb_count = tb_count;
    tu_head.first_pc = tb_vector[0]->pc;
    tu_head.tu_sum_size = sizeof(tu_head) + tb_vector[0]->s_data->tu_size;

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        if (i && tb->s_data->is_first_tb) {
            fprintf(stderr, "[AOT] Save: nested TU entry while serializing\n");
            return false;
        }
        remove_misc_flags(tb, MF_LINK0_MASK | MF_LINK1_MASK | MF_LINK_STATUS);
        /* Keep the serialized software-RAS gate closed but relinkable. */
        if (option_soft_ras && tb->inst_ptr_offset) {
            g_assert(latxs_ras_close_relinkable(tb));
        }
#if DEBUG_AOT
        ++out_tb_file_tb_count;
#endif
        if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 0);
        }
        if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 1);
        }
        if (TB_REL_START(tb) != -1) {
            tu_head.rel_count += TB_REL_END(tb) - TB_REL_START(tb) + 1;
        }
        tu_head.tu_sum_size += option_split_search ?
            tb->icount << 2 : tb->search_size;
        if (option_pushpop) {
            tu_head.tu_sum_size += tb->icount << 2;
        }
    }

    tu_head.page_count = mapping_count;
    tu_head.tu_id = aot_make_tu_id(tb_vector, tb_count, tu_pages,
            tu_head.page_count);

    tu_head.tu_sum_size += sizeof(aot_block) * tu_head.tb_count
        + sizeof(aot_rel) * tu_head.rel_count
        + sizeof(aot_tu_page) * tu_head.page_count;
    if (tu_head.rel_count) {
        saved_rels = g_try_new(aot_rel, tu_head.rel_count);
        if (!saved_rels) {
            return false;
        }
    }

    if (!aot_codec_write(file, &tu_head, sizeof(tu_head), 1) ||
            !aot_codec_write(file, tu_pages, sizeof(*tu_pages),
                            tu_head.page_count)) {
        goto out;
    }

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];
        aot_block aot_tb = { 0 };

        aot_encode_block(&aot_tb, tb);
        if (TB_REL_START(tb) != -1) {
            int rel_count = TB_REL_END(tb) - TB_REL_START(tb) + 1;

            aot_tb.rel_start = saved_rel_count;
            aot_tb.rel_end = saved_rel_count + rel_count - 1;
            for (int j = 0; j < rel_count; j++) {
                saved_rels[saved_rel_count] =
                    tu_rel_table[TB_REL_START(tb) + j];
                saved_rels[saved_rel_count].tc_offset -= curr_ir2_size;
                saved_rel_count++;
            }
        } else {
            aot_tb.rel_start = -1;
            aot_tb.rel_end = -2;
        }
        if (!aot_codec_write(file, &aot_tb, sizeof(aot_tb), 1)) {
            goto out;
        }
        curr_ir2_size += tb->ir2_num * 4;
    }
    g_assert(saved_rel_count == tu_head.rel_count);

    if (!aot_codec_write(file, tb_vector[0]->tc.ptr,
                         tb_vector[0]->s_data->tu_size, 1) ||
            (tu_head.rel_count &&
             !aot_codec_write(file, saved_rels, sizeof(*saved_rels),
                             tu_head.rel_count))) {
        goto out;
    }

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        if (!option_split_search) {
            if (!aot_codec_write(file, (void *)(ADDR)tb->tc.ptr +
                                (tb->ir2_num << 2), tb->search_size, 1)) {
                goto out;
            }
        } else if (!aot_codec_write(file, (void *)(ADDR)tb->search_ptr,
                                   tb->icount * 4, 1)) {
            goto out;
        }
    }
    if (option_pushpop) {
        for (int i = 0; i < tb_count; i++) {
            TranslationBlock *tb = tb_vector[i];

            if (!aot_codec_write(file, (void *)(ADDR)tb_opt_ptr(tb),
                                tb->icount * 4, 1)) {
                goto out;
            }
        }
    }

    *record_size += tu_head.tu_sum_size;
    if (tu_id) {
        *tu_id = tu_head.tu_id;
    }
    ok = true;

out:
    g_free(saved_rels);
    return ok;
}

#endif
