/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "latx-tu.h"
#include "accel/tcg/internal.h"
#include <syscall.h>

static bool option_check_tu;
static __thread uint32_t *temp_buf;
static __thread int temp_buf_size;

void aot_ras_clear(TranslationBlock *tb)
{
    if (!option_soft_ras || !tb || !tb->inst_ptr_offset) {
        return;
    }
    uint32_t *inst_ptr;
    uint32_t nop = 0x03400000;
#ifdef CONFIG_LATX_TU
    if (tb_is_tu(tb)) {
        inst_ptr = (uint32_t *)((uintptr_t)tb->tc.ptr +
                tb->inst_ptr_offset - tb->tc.offset_in_tu);
    } else {
        inst_ptr = (uint32_t *)((uintptr_t)tb->tc.ptr + tb->inst_ptr_offset);
    }
#else
    inst_ptr = (uint32_t *)((uintptr_t)tb->tc.ptr + tb->inst_ptr_offset);
#endif
    inst_ptr[4] = nop;
    inst_ptr[3] = nop;
    inst_ptr[2] = nop;
    inst_ptr[1] = nop;
    inst_ptr[0] = nop;
    inst_ptr[-1] = nop;
    inst_ptr[-2] = 0x50000000 | (0x7 << 10);
}

static void save_ir2_to_temp(TranslationBlock *tb)
{
    uint32_t ir2_size = tb->ir2_num << 2;
    int search_size;
    if (option_split_search) {
        search_size = tb->icount << 2;
    } else {
        search_size = tb->search_size;
    }
    if (temp_buf_size <= ir2_size + search_size) {
        free(temp_buf);
        temp_buf_size = ir2_size + search_size;
        temp_buf = malloc(temp_buf_size);
    }
    if (option_split_search) {
        memcpy((void *)temp_buf, (void *)tb->tc.ptr, ir2_size);
        memcpy((void *)temp_buf + ir2_size, tb->search_ptr, search_size);
    } else {
        memcpy((void *)temp_buf, (void *)tb->tc.ptr, ir2_size + tb->search_size);
    }
}

/* not check search_ptr, maybe have error. */
void check_aot_tb(TranslationBlock *curr_tb, TranslationBlock *tmp_tb)
{

    if (curr_tb->tc.ptr != tmp_tb->tc.ptr) {
        fprintf(stderr, "not equal tcptr %p %p\n", curr_tb->tc.ptr, tmp_tb->tc.ptr);
    }
    if (curr_tb->pc != tmp_tb->pc ) {
        fprintf(stderr, "not equal pc  %lx %lx \n", curr_tb->pc , tmp_tb->pc );
        abort();
    }
    /* if (curr_tb->search_size != tmp_tb->search_size ) { */
    /*     fprintf(stderr, "not equal  %lx %lx \n", curr_tb->search_size , tmp_tb->search_size ); */
    /*     exit(0); */
    /* } */
    if (curr_tb->hva != tmp_tb->hva ) {
        fprintf(stderr, "not equal hva  %lx %lx \n", curr_tb->hva , tmp_tb->hva );
        abort();
    }
    if (curr_tb->cs_base != tmp_tb->cs_base ) {
        fprintf(stderr, "not equal cs_base %x %x \n",
                (uint32_t)curr_tb->cs_base, (uint32_t)tmp_tb->cs_base );
        abort();
    }
    if (curr_tb->ir2_num != tmp_tb->ir2_num ) {
        fprintf(stderr, "not equal ir2_num %d %d \n", curr_tb->ir2_num , tmp_tb->ir2_num );
        abort();
    }
    if (curr_tb->preorder_tb_offset != tmp_tb->preorder_tb_offset ) {
        fprintf(stderr, "not equal preorder_tb_offset %x %x \n",
                curr_tb->preorder_tb_offset , tmp_tb->preorder_tb_offset );
        abort();
    }
    if (curr_tb->inst_ptr_offset != tmp_tb->inst_ptr_offset ) {
        fprintf(stderr, "not equal inst_ptr_offset  %x %x \n",
                curr_tb->inst_ptr_offset , tmp_tb->inst_ptr_offset );
        abort();
    }
    if (curr_tb->label_pos_nocheck != tmp_tb->label_pos_nocheck ) {
        fprintf(stderr, "not equal label_pos_nocheck %x %x \n",
                curr_tb->label_pos_nocheck , tmp_tb->label_pos_nocheck );
        abort();
    }
    if (curr_tb->size != tmp_tb->size ) {
        fprintf(stderr, "not equal size %x %x \n", curr_tb->size , tmp_tb->size );
        abort();
    }
    if (curr_tb->icount != tmp_tb->icount ) {
        fprintf(stderr, "not equal icount  %d %d \n", curr_tb->icount , tmp_tb->icount );
        abort();
    }

    if (curr_tb->page_addr[0] != tmp_tb->page_addr[0] ) {
        fprintf(stderr, "not equal page_addr0 %lx %lx \n",
                curr_tb->page_addr[0] , tmp_tb->page_addr[0] );
        abort();
    }
    if (curr_tb->page_addr[1] != tmp_tb->page_addr[1] ) {
        fprintf(stderr, "not equal page_addr[1] %lx %lx \n",
                curr_tb->page_addr[1] , tmp_tb->page_addr[1] );
        abort();
    }
    if (curr_tb->misc_flags != tmp_tb->misc_flags ) {
        fprintf(stderr, "not equal misc_flags  %x %x \n",
                curr_tb->misc_flags , tmp_tb->misc_flags );
        fprintf(stderr, "pc %lx flags %x %x\n",
                curr_tb->pc, curr_tb->flags, tmp_tb->flags);
        abort();
    }
    if (curr_tb->jmp_lock.value != tmp_tb->jmp_lock.value ) {
        fprintf(stderr, "not equal jmp_lock  %x %x \n",
                curr_tb->jmp_lock.value , tmp_tb->jmp_lock.value );
        abort();
    }
    if (curr_tb->jmp_list_head != tmp_tb->jmp_list_head ) {
        fprintf(stderr, "not equal jmp_list_head %lx %lx \n",
                curr_tb->jmp_list_head , tmp_tb->jmp_list_head );
        abort();
    }
    if (curr_tb->jmp_dest[0] != tmp_tb->jmp_dest[0] ) {
        fprintf(stderr, "not equa jmp_dest0 %lx %lx \n",
                curr_tb->jmp_dest[0] , tmp_tb->jmp_dest[0] );
        abort();
    }
    if (curr_tb->jmp_dest[1] != tmp_tb->jmp_dest[1] ) {
        fprintf(stderr, "not equal jmp_dest1 %lx %lx \n",
                curr_tb->jmp_dest[1] , tmp_tb->jmp_dest[1] );
        abort();
    }
    if (curr_tb->jmp_reset_offset[0] != tmp_tb->jmp_reset_offset[0] ) {
        fprintf(stderr, "not equal jmp_reset_offset0 %x %x \n",
                curr_tb->jmp_reset_offset[0] , tmp_tb->jmp_reset_offset[0] );
        abort();
    }
    if (curr_tb->jmp_reset_offset[1] != tmp_tb->jmp_reset_offset[1] ) {
        fprintf(stderr, "not equal jmp_reset_offset1  %x %x \n",
                curr_tb->jmp_reset_offset[1] , tmp_tb->jmp_reset_offset[1] );
        abort();
    }
    if (curr_tb->jmp_insn_offset[0] != tmp_tb->jmp_insn_offset[0] ) {
        fprintf(stderr, "not equal jmp_insn_offset0 %x %x \n",
                curr_tb->jmp_insn_offset[0] , tmp_tb->jmp_insn_offset[0] );
        abort();
    }
    if (curr_tb->jmp_insn_offset[1] != tmp_tb->jmp_insn_offset[1] ) {
        fprintf(stderr, "not equal jmp_insn_offset1 %x %x \n",
                curr_tb->jmp_insn_offset[1] , tmp_tb->jmp_insn_offset[1] );
        abort();
    }
    if (curr_tb->jmp_target_addr[0] != tmp_tb->jmp_target_addr[0] ) {
        fprintf(stderr, "not equal jmp_target_arg[0] %lx %lx \n",
                curr_tb->jmp_target_addr[0] , tmp_tb->jmp_target_addr[0] );
        abort();
    }
    if (curr_tb->jmp_target_addr[1] != tmp_tb->jmp_target_addr[1] ) {
        fprintf(stderr, "not equal jmp_target_addr[1] %lx %lx \n",
                curr_tb->jmp_target_addr[1] , tmp_tb->jmp_target_addr[1] );
        abort();
    }
    if (curr_tb->flags != tmp_tb->flags ) {
        fprintf(stderr, "not equal flags %x %x \n",
                curr_tb->flags , tmp_tb->flags );
        abort();
    }
    /* if (curr_tb->cflags != tmp_tb->cflags ) { */
    /*     fprintf(stderr, "not equal cflag %lx %lx \n", */
    /*             curr_tb->cflags , tmp_tb->cflags ); */
    /*     exit(0); */
    /* } */
    if (curr_tb->jmp_cp_data[0] != tmp_tb->jmp_cp_data[0] ) {
        fprintf(stderr, "not equal jmp_cp_data[0] %x %x \n",
                curr_tb->jmp_cp_data[0], tmp_tb->jmp_cp_data[0]);
        abort();
    }
    if (curr_tb->jmp_cp_data[1] != tmp_tb->jmp_cp_data[1] ) {
        fprintf(stderr, "not equal jmp_cp_data[1] %x %x \n",
                curr_tb->jmp_cp_data[1] , tmp_tb->jmp_cp_data[1] );
        abort();
    }
    if (curr_tb->jmp_inst_hold[0] != tmp_tb->jmp_inst_hold[0] ) {
        fprintf(stderr, "not equal jmp_inst_hold[0] %lx %lx \n",
                curr_tb->jmp_inst_hold[0] , tmp_tb->jmp_inst_hold[0] );
        abort();
    }
    if (curr_tb->jmp_inst_hold[1] != tmp_tb->jmp_inst_hold[1] ) {
        fprintf(stderr, "not equal jmp_insn_hold[1] %lx %lx \n",
                curr_tb->jmp_inst_hold[1] , tmp_tb->jmp_inst_hold[1] );
        abort();
    }
    if (curr_tb->link_opt[0] != tmp_tb->link_opt[0] ) {
        fprintf(stderr, "not equal link_opt[0] %x %x \n",
                curr_tb->link_opt[0] , tmp_tb->link_opt[0] );
        abort();
    }
    if (curr_tb->link_opt[1] != tmp_tb->link_opt[1] ) {
        fprintf(stderr, "not equal link_opt[1] %x %x \n",
                curr_tb->link_opt[1] , tmp_tb->link_opt[1] );
        abort();
    }
    if (curr_tb->next_pc[0] != tmp_tb->next_pc[0] ) {
        fprintf(stderr, "not equal next_pc  %lx %lx \n",
                curr_tb->next_pc[0] , tmp_tb->next_pc[0] );
        abort();
    }
    if (curr_tb->next_pc[1] != tmp_tb->next_pc[1] ) {
        fprintf(stderr, "not equal next_pc[1]  %lx %lx \n",
                curr_tb->next_pc[1] , tmp_tb->next_pc[1] );
        abort();
    }
    if (curr_tb->preorder_next != tmp_tb->preorder_next ) {
        fprintf(stderr, "not equal preorder_next %p %p \n",
                curr_tb->preorder_next , tmp_tb->preorder_next );
        abort();
    }
    fprintf(stderr, "check tb ok\n");

}


bool check_ir2(TranslationBlock *curr_tb, aot_block *file_tb)
{
    assert(file_tb);
    assert(file_tb->pc == curr_tb->pc);
    static int ok_cnt = 0;
    if (1 || curr_tb->ir2_num == (file_tb->ir2_num)) {
        uint32_t* code = (uint32_t*)(curr_tb->tc.ptr);
        /* uint32_t* file_code = (uint32_t*)((void *)(uintptr_t)(file_tb + 1)); */
        uint32_t* file_code = temp_buf;

        for (int i = 0; i < curr_tb->ir2_num; i++) {
            if (code[i] != file_code[i]) {
                    fprintf(stderr, "check fail i %d addr %p %08x %08x tid %ld\n",
                            i, &code[i], code[i], file_code[i],
                            syscall(SYS_gettid));
                fprintf(stderr, "------------------------- ir2_num curr_tb %d file_tb %d\n",
                        curr_tb->ir2_num, file_tb->ir2_num);
                for (int j = i; j < curr_tb->ir2_num; j++) {
                     fprintf(stderr, "============ check fail j %d addr %p %08x %08x\n",
                            j, &code[j], code[j], file_code[j]);
                }

                int  ff= 3;
                while(ff--) {
                    fprintf(stderr, "check fail i %d addr %p %p %08x %08x tid %ld\n",
                            i, &code[i], &file_code[i], code[i], file_code[i],
                            syscall(SYS_gettid));

                    /* sleep(1); */
                    fprintf(stderr, "pid %d\n", getpid());
                    printf("hhhh\n");
                }
                return false;
                abort();
            }
        }
    } else {
        fprintf(stderr, "tb code size mismatch fail fail fail fail fail fail fail fail fail fail fail fail fail fail \n");
        fprintf(stderr, "tb->ir2_num:%d, file_tb->ir2_num:%d \n", curr_tb->ir2_num, (file_tb->ir2_num));
        // for (int i = 0; i < tb->size; i++) {
        //     fprintf(stderr, "%02x ", *(uint8_t*)(tb->tc.ptr + i));
        // }
        // fprintf(stderr, "\n");
        for (int i = 0; i < (curr_tb->ir2_num); i++) {
            uint32_t* code = (uint32_t*)(curr_tb->tc.ptr);
            fprintf(stderr, "i %d %08x %08x\n", i, code[i], temp_buf[i]);
        }
        fprintf(stderr, "\n");
        abort();
    }
    fprintf(stderr, "check ok\n");
    ok_cnt++;
    return true;
}

TranslationBlock *gen_check_tb(TranslationBlock *tb, aot_block *file_tb,
        CPUArchState *env, CPUState *cpu)
{
    aot_ras_clear(tb);
    uint64 pc = tb->pc;
    save_ir2_to_temp(tb);
    curr_rel_entry_num = 0;
    void *host_pc = NULL;
    int search_size;
    int max_insns = tb->cflags & CF_COUNT_MASK;
    if (max_insns == 0) {
        max_insns = TCG_MAX_INSNS;
    }
    latx_setjmp_gen_code(cpu, tb, pc, host_pc, max_insns, &search_size);
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 0);
        } else {
            tb_reset_jump(tb, 0);
        }
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 1);
        } else {
            tb_reset_jump(tb, 1);
        }
    }
    aot_ras_clear(tb);
    if (!check_ir2(tb, file_tb)) {
        fprintf(stderr, "file_tb pc %lx curr tb pc %lx\n", file_tb->pc, tb->pc);
        fprintf(stderr, "===========================\n");
        option_dump = 1;
        option_dump_host = 1;
        option_dump_ir1 = 1;
        option_dump_ir2 = 1;
        option_dump_cpl3 = 0;
        latx_setjmp_gen_code(cpu, tb, pc, host_pc, max_insns, &search_size);
        option_dump = 0;
        option_dump_host = 0;
        option_dump_ir1 = 0;
        option_dump_ir2 = 0;
        option_dump_cpl3 = 0;
        int ff = 10;
        while(ff--) {
            fprintf(stderr, "1111 hhh\n");
            sleep(1);
        }
    }
    /* if (tmp_tb) { */
    /*     check_aot_tb(tb, tmp_tb); */
    /*     return tmp_tb; */
    /* } */
    /* return NULL; */
    return tb;
}

void check_tu(CPUState *cpu, aot_tu_head *tu_head, TranslationBlock **curr_tb_vector, aot_block *file_tb_vector, uintptr_t tbmini_ptr, int ir2_size)
{
    TranslationBlock *tb;
    if (option_check_tu) {
        for (int i = 0; i < tu_head->tb_count; i++) {
            aot_ras_clear(curr_tb_vector[i]);
        }

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
        qatomic_set(&tcg_ctx->code_gen_ptr, tbmini_ptr);
#endif
        tb = curr_tb_vector[0];
        int ir2_num = tb->ir2_num;
        tb->ir2_num = ir2_size / 4;
        save_ir2_to_temp(curr_tb_vector[0]);
        tb->ir2_num = ir2_num;
        tb = gen_check_tu(cpu, curr_tb_vector, tu_head->tb_count);
        tb->ir2_num = tb->s_data->tu_size / 4;
        fprintf(stderr, "check tc ptr %p icount %d\n",
                tb->tc.ptr, tb->icount);
        if (!check_ir2(tb, &file_tb_vector[0])) {
            option_dump_ir1 = 1;
            option_dump = 1;
            option_dump_host = 1;
            option_dump_ir1 = 1;
            option_dump_ir2 = 1;
            option_dump_cpl3 = 1;
            tb = gen_check_tu(cpu, curr_tb_vector, tu_head->tb_count);
            int ff = 10;
            while (ff) {
                fprintf(stderr, "pid %d\n", getpid());
                sleep(1);
            }
        }
        tb->ir2_num = ir2_num;
    }
}
