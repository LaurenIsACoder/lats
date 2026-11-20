/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "env.h"
#include "error.h"
#include "exec/cpu-all.h"
#include "ir2.h"
#include "latx-options.h"
#include "latxs-imm-cache.h"
#include "lsenv.h"
#include "qemu/compiler.h"
#include "qemu/log.h"
#include "qemu/osdep.h"
#include "x86.h"
#include "latx-sys-inst-ptn.h"

static __thread IMM_CACHE *imm_cache;

//reg related
__thread int cache_itemp_use_stat[4] = {1, 1, 1, 1};

FILE *log_file;
FILE *trace_file;
FILE *log_pc_file;

static FILE *imm_open_debug_file(const char *kind)
{
    char path[128];
    FILE *file;

    snprintf(path, sizeof(path), "/tmp/lats-imm-cache-%s-%ld.log", kind,
             (long)getpid());
    file = fopen(path, "w+");
    if (file == NULL) {
        fprintf(stderr, "imm cache debug disabled: cannot open %s: %s\n",
                path, strerror(errno));
        option_imm_debug = 0;
        return NULL;
    }
    setvbuf(file, NULL, _IOLBF, 0);
    return file;
}

static const char *registers[] = {
    "rax",
    "rcx",
    "rdx",
    "rbx",
    "rsp",
    "rbp",
    "rsi",
    "rdi",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "r13",
    "r14",
    "r15",
};
/**
 * itemp_stat index map itemp
 * x32: itemp9 to itemp6
 * x64: itemp6 to itemp3
 */
const int cache_itemp_map[4] = {
    [0] = 2,
    [1] = 3,
    [2] = 4,
    [3] = 5,
};

/**
 * latxs_temp_index <-> cache_itemp_index
 */
const int latxs_itemp_reverse_map[6] = {
    [0 ... 5] = -1,
    [5] = 3,
    [4] = 2,
    [3] = 1,
    [2] = 0,
};

//=====================
// statistic analysis
//=====================
// long cache_call = 0;
// long cache_hit = 0;

// ========================
//      util functions
// ========================

/**
 * when function is called, free itemp must exist
 * allocate from
 * array reflection: 3-0:
 * to avoid conflict
 */
static inline int alloc_cache_itemp(void)
{
    for (int i = 3; i >= 0; i--) {
        if (cache_itemp_use_stat[i]) {
            cache_itemp_use_stat[i] = 0;
            return cache_itemp_map[i];
        }
    }
    lsassert(0);
    return -1;
}

static inline void free_cache_itemp(int itemp_index)
{
    /**
     * itemp_num is
     * x32: [7-10] -> [0-3]
     * x64: [3-6] -> [0-3]
     */
    int index = latxs_itemp_reverse_map[itemp_index];
    cache_itemp_use_stat[index] = 1;
    // CPUState *env = env_cpu(lsenv->cpu_state);
    imm_log("[%d]imm_cache free itemp :%d\n", qemu_get_thread_id(), itemp_index);
}

bool imm_cache_is_using_temp(int itemp_index)
{
    if (imm_cache == NULL)
        return false;
    if (itemp_index < 0 || itemp_index > 5) {
        return false;
    }
    int cache_temp_index = latxs_itemp_reverse_map[itemp_index];
    if (cache_temp_index == -1) {
        return false;
    }
    if (cache_itemp_use_stat[cache_temp_index] == 0) {
        return true;
    }
    return false;
}

// bool imm_cache_is_cached_at(int cache_index) { return !imm_cache->bucket[cache_index].free; }

// int imm_cache_la_reg_num_at(int cache_index)
// {
//     // return itemp4
//     int itemp_num = imm_cache->bucket[cache_index].itemp_index;

//     int reg_num = cache_itemp_map[itemp_num];
//     return reg_num;
// }
// static inline int compare_imm_cache(const void *a, const void *b)
// {
//     return ((IMM_CACHE_BUCKET *)b)->use_count - ((IMM_CACHE_BUCKET *)a)->use_count;
// }

// sort by use frequency, range: cache_count
inline void imm_cache_sort(void)
{
    IMM_CACHE_BUCKET *buckets = imm_cache->bucket;
    int count = imm_cache->cache_count;

    for (int i = 1; i < count; i++) {
        IMM_CACHE_BUCKET key = buckets[i];
        int j = i - 1;

        while (j >= 0 && buckets[j].use_count < key.use_count) {
            buckets[j + 1] = buckets[j];
            j--;
        }
        buckets[j + 1] = key;
    }
}

// ============================
//    cache init functions
// ============================
static inline void imm_cache_init_bucket(IMM_CACHE_BUCKET *bucket, int index)
{
    //======normal cache ======
    bucket[index].free = true;
    bucket[index].use_count = 0;
    bucket[index].itemp_index = -1;
    bucket[index].base = -1;
    bucket[index].index = -1;
    bucket[index].scale = -1;
    bucket[index].curr_ir1_index = -1;
    bucket[index].offset = 0;
    //======pre cache======
    bucket[index].lifecycle = 0;
    bucket[index].pre_cache = false;
    bucket[index].avg_offset = 0;
    bucket[index].min_offset = LONG_LONG_MAX;
    bucket[index].max_offset = LONG_LONG_MIN;
}

void imm_cache_init(int capacity)
{
    lsassertm((imm_cache != NULL), "imm_cache memory is NULL\n");
    /*capacity should > CACHE_DEFAULT_CAPACITY*/
    if (capacity < CACHE_DEFAULT_CAPACITY) {
        capacity = CACHE_DEFAULT_CAPACITY;
    }

    imm_cache->cache_count = capacity;
    imm_cache->free_count = imm_cache->cache_count;
    imm_cache->curr_imm_index = 0;
    imm_cache->curr_ir1_index = 0;
    imm_cache->curr_ir2_index = 0;
    imm_cache->optimized_ir2 = false;
    imm_cache->itemp_allocated = false;
    imm_cache->opt_take_effect = false;
    imm_cache->tb_start = true;
    for (int i = 0; i < 16; i++) {
        imm_cache->ir1_reg_last_updated_index[i] = -1;
    }
    lsassertm((imm_cache->bucket != NULL), "imm_cache buckets memory is NOT malloced \n");
    for (int i = 0; i < 4; i++) {
        cache_itemp_use_stat[i] = 1;
    }
    for (int i = 0; i < imm_cache->cache_count; i++) {
        imm_cache_init_bucket(imm_cache->bucket, i);
    }
    // CPUState *env = env_cpu(lsenv->cpu_state);
    // imm_log("[%d]imm_cache init\n", qemu_get_thread_id());
}

void imm_cache_replace_bucket(int cache_id, int base, int index, int scale, int64 offset)
{
    // pre cache changed to cached
    IMM_CACHE_BUCKET *bucket = &(imm_cache->bucket[cache_id]);
    imm_cache_fill_bucket(cache_id, base, index, scale, offset);
    bucket->lifecycle = 0;
    bucket->pre_cache = false;
    bucket->curr_ir1_index = imm_cache->curr_ir1_index;
    bucket->use_count = 1;
}

void imm_cache_fill_bucket(int cache_id, int base, int index, int scale, int64 offset)
{
    /* new bucket data */
    lsassertm((cache_id < imm_cache->cache_count),
              "imm_cache_fill_bucket fail: cache_id %d > cache_count %d fail \n",
              cache_id,
              imm_cache->cache_count);

    imm_cache->bucket[cache_id].base = base;
    imm_cache->bucket[cache_id].index = index;
    imm_cache->bucket[cache_id].scale = scale;
    imm_cache->bucket[cache_id].offset = offset;

    imm_cache->bucket[cache_id].avg_offset = 0;

    imm_cache->bucket[cache_id].free = false;
    imm_cache->bucket[cache_id].use_count = 0;
}

/**
 * TODO: some instructions will change gpr indirectly
 */
void imm_cache_update_ir1_usage(IR1_INST *pir1, int curr_ir1_index)
{
    // imm_cache_update_all_reg_ir1_usage(pir1, curr_ir1_index);
    /* trace x86_reg latest define value */
    IR1_OPCODE opcode = ir1_opcode(pir1);
    int *ir1_updated_index = imm_cache->ir1_reg_last_updated_index;
    switch (opcode) {
    case X86_INS_PUSH:
    case X86_INS_PUSHF:
    case X86_INS_PUSHFD:
    case X86_INS_PUSHFQ:
    case X86_INS_PUSHAL:
    case X86_INS_PUSHAW:

    case X86_INS_POP:
    case X86_INS_POPF:
    case X86_INS_POPFD:
    case X86_INS_POPFQ:
        ir1_updated_index[esp_index] = curr_ir1_index;
        break;
    case X86_INS_LODSB:
    case X86_INS_LODSW:
    case X86_INS_LODSD:
    case X86_INS_LODSQ:
    case X86_INS_OUTSB:
    case X86_INS_OUTSW:
    case X86_INS_OUTSD:
        ir1_updated_index[esi_index] = curr_ir1_index;
        if (latxs_ir1_has_prefix_repe(pir1) ||
               latxs_ir1_has_prefix_repne(pir1))
            ir1_updated_index[ecx_index] = curr_ir1_index;
        break;
    case X86_INS_STOSB:
    case X86_INS_STOSW:
    case X86_INS_STOSD:
    case X86_INS_STOSQ:
    case X86_INS_SCASB:
    case X86_INS_SCASW:
    case X86_INS_SCASD:
    case X86_INS_SCASQ:
    case X86_INS_INSB:
    case X86_INS_INSW:
    case X86_INS_INSD:
        ir1_updated_index[edi_index] = curr_ir1_index;
        if (latxs_ir1_has_prefix_repe(pir1) ||
               latxs_ir1_has_prefix_repne(pir1))
            ir1_updated_index[ecx_index] = curr_ir1_index;
        break;
    case X86_INS_MOVSB:
    case X86_INS_MOVSW:
    case X86_INS_MOVSD:
    case X86_INS_MOVSQ:
    case X86_INS_CMPSB:
    case X86_INS_CMPSW:
    case X86_INS_CMPSD:
    case X86_INS_CMPSQ:
        ir1_updated_index[esi_index] = curr_ir1_index;
        ir1_updated_index[edi_index] = curr_ir1_index;
        if (latxs_ir1_has_prefix_repe(pir1) ||
               latxs_ir1_has_prefix_repne(pir1))
            ir1_updated_index[ecx_index] = curr_ir1_index;
        break;
    case X86_INS_MUL:
    case X86_INS_IMUL:
    case X86_INS_DIV:
    case X86_INS_IDIV:
        ir1_updated_index[eax_index] = curr_ir1_index;
        ir1_updated_index[edx_index] = curr_ir1_index;
        break;
    case X86_INS_MULX:
        /* MULX writes both explicit destination operands. */
        for (int i = 0; i < 2; i++) {
            IR1_OPND *opnd = ir1_get_opnd(pir1, i);
            if (ir1_opnd_is_gpr(opnd)) {
                int reg_num = ir1_opnd_base_reg_num(opnd);
                ir1_updated_index[reg_num] = curr_ir1_index;
            }
        }
        break;
    // both src and dest will change
    case X86_INS_XCHG:
    case X86_INS_XADD:
        for (int i = 0; i < 2; i++) {
            IR1_OPND *opnd0 = ir1_get_opnd(pir1, i);
            if (ir1_opnd_is_gpr(opnd0)) {
                int reg_num = ir1_opnd_base_reg_num(opnd0);
                ir1_updated_index[reg_num] = curr_ir1_index;
            }
        }
        return;
        break;
    case X86_INS_AAA:
    case X86_INS_AAS:
    case X86_INS_DAA:
    case X86_INS_DAS:
    case X86_INS_AAM:
    case X86_INS_AAD:
    case X86_INS_XLATB:
    case X86_INS_CBW:
    case X86_INS_CWDE:
    case X86_INS_CDQE:
    case X86_INS_CMPXCHG:
    case X86_INS_LAHF:
    case X86_INS_SAHF:
        ir1_updated_index[eax_index] = curr_ir1_index;
        break;
    case X86_INS_CWD:
    case X86_INS_CDQ:
    case X86_INS_CQO:
        ir1_updated_index[edx_index] = curr_ir1_index;
        break;
    case X86_INS_CMPXCHG8B:
    case X86_INS_CMPXCHG16B:
    case X86_INS_RDTSC:
        ir1_updated_index[eax_index] = curr_ir1_index;
        ir1_updated_index[edx_index] = curr_ir1_index;
        break;
    case X86_INS_RDTSCP:
        ir1_updated_index[eax_index] = curr_ir1_index;
        ir1_updated_index[edx_index] = curr_ir1_index;
        ir1_updated_index[ecx_index] = curr_ir1_index;
        break;
    case X86_INS_CPUID:
        ir1_updated_index[eax_index] = curr_ir1_index;
        ir1_updated_index[ebx_index] = curr_ir1_index;
        ir1_updated_index[ecx_index] = curr_ir1_index;
        ir1_updated_index[edx_index] = curr_ir1_index;
        break;
    // all reg changes
    case X86_INS_POPAL:
    case X86_INS_POPAW:
        for (int i = 0; i < 8; i++) {
            ir1_updated_index[i] = curr_ir1_index;
        }
        break;
    case X86_INS_CALL:
    case X86_INS_LCALL:
    case X86_INS_RET:
    case X86_INS_INT:
    case X86_INS_INTO:
    case X86_INS_INT1:
    case X86_INS_INT3:
    case X86_INS_IRET:
    case X86_INS_IRETD:
    case X86_INS_IRETQ:
    case X86_INS_SYSENTER:
    case X86_INS_SYSEXIT:
    case X86_INS_SYSCALL:
    case X86_INS_SYSRET:
    case X86_INS_RETF:
    case X86_INS_RETFQ:

    case X86_INS_JMP:
    case X86_INS_LJMP:

    //pop
    case X86_INS_ENTER:
    case X86_INS_LEAVE:
    //other
        for (int i = 0; i < 16; i++) {
            ir1_updated_index[i] = curr_ir1_index;
        }
        break;
    default:
        break;
    }
    if (ir1_opnd_num(pir1)) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_gpr(opnd0)) {
            int reg_num = ir1_opnd_base_reg_num(opnd0);
            ir1_updated_index[reg_num] = curr_ir1_index;
        }
    }
}

bool imm_cache_skip_curr_ir1(void)
{
    /* skip ir1 whose translation uses too much itemp */
    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;
    IR1_OPCODE cur_ir1_op = ir1_opcode(pir1);
    switch (cur_ir1_op) {
    //push
    case X86_INS_PUSH:
    case X86_INS_PUSHAL:
    case X86_INS_PUSHAW:
    case X86_INS_PUSHF:
    case X86_INS_PUSHFD:
    case X86_INS_PUSHFQ:
    //pop
    case X86_INS_POP:
    case X86_INS_POPAW:
    case X86_INS_POPAL:
    case X86_INS_POPCNT:
    case X86_INS_POPF:
    case X86_INS_POPFD:
    case X86_INS_POPFQ:
    //ret
    case X86_INS_RET:
    case X86_INS_RETF:
    case X86_INS_RETFQ:
    //call
    case X86_INS_CALL:
    //enter
    case X86_INS_ENTER:
    //leave
    case X86_INS_LEAVE:
    //maskmovdqu
    case X86_INS_MASKMOVDQU:
    case X86_INS_MASKMOVQ:
    //other
    case X86_INS_FNSTENV:

    case X86_INS_FIST:
    case X86_INS_FISTP:

    // case X86_INS_XADD:
    case X86_INS_XCHG:
    case X86_INS_NEG:
    case X86_INS_ROL:
    case X86_INS_ROR:
    case X86_INS_RCL:
    case X86_INS_RCR:
    case X86_INS_SHRD:
    case X86_INS_SHLD:
    case X86_INS_CMPXCHG:
    case X86_INS_ARPL:
    // case X86_INS_SAR:
    // case X86_INS_SHR:
    // case X86_INS_ROL:
    // case X86_INS_ROR:
    // case X86_INS_SAL:
    // case X86_INS_SHL:
    // case X86_INS_RCL:
    // case X86_INS_FIST:
    // case X86_INS_FICOMP:
    // case X86_INS_RCR:
    // case X86_INS_FISTP:
        return true;
        break;
    default:
        break;
    }
    return false;
}

bool imm_cache_skip_for_instptr(void)
{
    if (option_instptn) {
        switch (lsenv->tr_data->curr_ir1_inst->instptn.opc)
        {
        case INSTPTN_OPC_CMP_XXCC_CON:
        case INSTPTN_OPC_TEST_XXCC_CON:
        case INSTPTN_OPC_NOP_FOR_EXP:

        case INSTPTN_OPC_UCOMISD_SETA:
            return true;
            break;
        default:
            break;
        }
    }
    return false;
}
// ========================
//    ir1 precache functions
// ========================

/**
 * precache_put
 * new avg_offset may exceed si12 range
 * 1. base(-100) indicates rip
 * put and update avg_offset
 * check whether new avg_offset can be reached by min and max offset
 */
// void imm_cache_precache_put(int base, int index, int scale, int64 offset)
// {
//     imm_log(
//         "[precache put] base:%4d\tindex:%2d\tscale:%2d\toffset:0x%lx", base, index, scale, offset);

//     int cache_id = imm_cache_get(base, index, scale, offset);
//     if (cache_id >= 0) {
//         // check if new_avg_offset will make cache si12 overflow
//         int64 min_offset = MIN(offset, imm_cache->bucket[cache_id].min_offset);
//         int64 max_offset = MAX(offset, imm_cache->bucket[cache_id].max_offset);
//         long new_avg_offset = (min_offset + max_offset) / 2;
//         long diff = max_offset - new_avg_offset;
//         if (si12_overflow(diff)) {
//             /* new offset will make cache invalid(si12 oaverflow) */
//             imm_log("[precache put hit but overflow]:%d", cache_id);
//             cache_id = -2;
//         }
//     }

//     if (cache_id == -1 || cache_id == -2) {
//         // new pre cache
//         cache_id = imm_cache->cache_count - imm_cache->free_count;
//         imm_log("\t[new]:%d\n", cache_id);
//         imm_cache_fill_bucket(cache_id, base, index, scale, offset);
//         imm_cache->free_count--;

//         imm_cache->bucket[cache_id].curr_ir1_index = imm_cache->curr_ir1_index;
//     } else {
//         // exist
//         imm_log("[precache put hit]:%d\n", cache_id);
//     }
//     /*new or old bucket update these fields*/
//     int64 min_offset = MIN(offset, imm_cache->bucket[cache_id].min_offset);
//     int64 max_offset = MAX(offset, imm_cache->bucket[cache_id].max_offset);
//     long new_avg_offset = (min_offset + max_offset) / 2;
//     imm_cache->bucket[cache_id].avg_offset = new_avg_offset;
//     imm_cache->bucket[cache_id].min_offset = min_offset;
//     imm_cache->bucket[cache_id].max_offset = max_offset;
//     imm_cache->bucket[cache_id].use_count++;
//     imm_cache->bucket[cache_id].lifecycle = imm_cache->curr_imm_index;
//     imm_cache->curr_imm_index++;
// }

/**
 * use data collected when precache ir1 list
 * strategy:
 * 1.sort cache
 * 2.calculate avg offset and check overflow
 *  2.1 only imm with base/index or rip needs avg_offset
 * 3.fix top3 imm in head of translate
 */
// void imm_cache_finish_precache(void)
// {
//     // sort by usage frequence
//     imm_cache_sort();
//     // reset cache_count
//     imm_cache->cache_count = CACHE_DEFAULT_CAPACITY;
//     imm_cache->curr_imm_index = 0;
//     imm_cache->curr_ir1_index = 0;
//     imm_cache->curr_ir2_index = 0;
//     imm_cache->optimized_ir2 = false;
//     imm_cache->itemp_allocated = false;
//     imm_cache->opt_take_effect = false;
//     imm_cache->free_count = 0;
//     // fix top3 cache clear up [3-capacity] bucket
//     for (int i = 0; i < imm_cache->cache_count; i++) {
//         if (imm_cache->bucket[i].free || i >= 3) {
//             imm_cache_init_bucket(imm_cache->bucket, i);
//             imm_cache->free_count++;
//         } else {
//             imm_cache->bucket[i].pre_cache = true;
//             imm_cache->bucket[i].itemp_index = alloc_cache_itemp();
//         }
//     }

//     imm_log("[precache finish]finish precache.\n");
//     imm_cache_print_cache();
// }

// ===============================
//   cache validation functions
// ===============================
bool imm_cache_can_use(int bucket_id, int base, int index)
{
    /*
     * 1. precache cache
     * 2. normal cache
     * */

    // if (cache->avg_offset == 0) {
    // } else {
    // }
    return !imm_cache_check_base_index_has_changed(bucket_id, base, index);
}

bool imm_cache_check_base_index_has_changed(int bucket_id, int base_reg_num, int index_reg_num)
{
    /* skip no 'base+index' imm case */
    if (base_reg_num < 0 && index_reg_num < 0) {
        return false;
    }

    // check whether base/index updated
    int last_update_base = -1;
    int last_update_index = -1;
    IMM_CACHE_BUCKET *bucket = &(imm_cache->bucket[bucket_id]);
    if (base_reg_num >= 0) {
        last_update_base = imm_cache->ir1_reg_last_updated_index[base_reg_num];
    }
    if (index_reg_num >= 0) {
        last_update_index = imm_cache->ir1_reg_last_updated_index[index_reg_num];
    }
    if (last_update_base >= bucket->curr_ir1_index || last_update_index >= bucket->curr_ir1_index) {
        imm_log("[reg change] ir1_index_cache:%d\t"
                "last_update_base:%d\t"
                "last_update_index:%d\t\n",
                bucket->curr_ir1_index,
                last_update_base,
                last_update_index);
        return true;
    }
    return false;
}

// ===============================
//   cache fetch/store functions
// ===============================

/**
 * return the cache index of the imm
 * return -1 if miss (including overflow)
 */
int imm_cache_get(int base, int index, int scale, int64 offset)
{
    for (int i = 0; i < imm_cache->cache_count; i++) {
        IMM_CACHE_BUCKET *cachei = &(imm_cache->bucket[i]);
        if (cachei->index == index && cachei->base == base && cachei->scale == scale &&
            cachei->free == false) {
            /**
             * use avg_offset during precache, use offset during translation
             */
            int64 criterion_offset = cachei->avg_offset == 0 ? cachei->offset : cachei->avg_offset;

            /* hit */
            long diff = (long)(offset - criterion_offset);
            if (si12_overflow(diff)) {
                // overflow
                // maybe overflowed this bucket but hit another.
                continue;
            } else {
                return i;
            }
        }
    }
    return -1;
}

static inline void imm_log_print_head(void)
{
    imm_log("=====================================\n");
    imm_log("|| IMM CACHE :   0x%lx ||\n", imm_cache->curr_pc);
    imm_log("=====================================\n");
}
static void imm_to_str(char *buff, int base, int index, int scale, int64 offset)
{
    //rip
    if (base == -100) {
        sprintf(buff, "0x%lx", offset);
        return;
    }

    if (base >= 0) {
        if (index >= 0) {
            // base + index*scale + offset
            sprintf(buff, "%s + %s * %d + 0x%lx", registers[base], registers[index], scale, offset);
        } else {
            // base + offset
            sprintf(buff, "%s + 0x%lx", registers[base], offset);
        }
    } else {
        // index*scale + offset
        if (index >= 0) {
            sprintf(buff, "%s * %d + 0x%lx", registers[index], scale, offset);
        }
    }
}
/**
 * 1. clean dead cache
 * 2. get cache > cache_id
 * 3. cache_id
 * 4.
 *
 */
IMM_CACHE_RES imm_cache_allocate(int base, int index, int scale, int64 offset)
{
    static bool stop_opt = false;
    // if (unlikely(imm_cache->free_count == 0))
    imm_cache_sort();

#ifdef LATXS_IMM_CACHE_DEBUG
    if (option_imm_debug) {
        const char *prefix = (base == -100) ? "rip" : "complex";
        char imm_str[50];
        imm_to_str(imm_str, base, index, scale, offset);
        if (imm_cache->tb_start) {
            imm_log_print_head();
            imm_cache->tb_start = false;
        }

        imm_log("[ALLOCATE] %7s [ %s ]\n", prefix, imm_str);
        // imm_log("[allocate %s] b:%d\ti:%d\ts:%d\toffset:0x%lx\n", prefix, base, index, scale, offset);
        imm_log("Before Allocate:\n");
        imm_cache_print_cache();
    }
#endif

    // dead cache auto free
    // imm_cache_free_dead_cache(cache);

    IMM_CACHE_RES res = {
        .offset = offset,
        .diff = 0,
        .pre_cache = false,
        .cached = false,
    };
    int cache_id = -1;

    if (likely(!stop_opt)) {
        cache_id = imm_cache_get(base, index, scale, offset);
    }
    if (cache_id == -1) {
        // a new cache
        imm_log("[LOG] new ");
        cache_id = imm_cache_put(base, index, scale, offset);
        res.cached = false;
    } else {
        // imm_log("\t[cache get]diff:%lx\n", diff);
        // cached, check validation
        if (imm_cache_can_use(cache_id, base, index)) {
            long diff = imm_cache_diff(cache_id, offset);
            imm_log("[LOG] hit cache_id=%d diff=%lx\n", cache_id, diff);
            if (imm_cache->bucket[cache_id].avg_offset == 0) {
                res.offset = imm_cache->bucket[cache_id].offset;
            } else {
                res.offset = imm_cache->bucket[cache_id].avg_offset;
            }
            if (imm_cache->bucket[cache_id].pre_cache) {
                imm_cache->bucket[cache_id].pre_cache = false;
                res.pre_cache = true;
            }
            res.diff = diff;
            res.cached = true;
            imm_cache->bucket[cache_id].use_count++;
            // cache_hit++;
        } else {
            // replace cache
            imm_log("[cache cannot use] replace cache_id:%d\n", cache_id);
            // imm_cache_init_bucket(cache->bucket, cache_id);
            imm_cache_replace_bucket(cache_id, base, index, scale, offset);
            res.offset = offset;
            res.cached = false;
        }
    }
    res.cache_id = cache_id;
    res.itemp_index = imm_cache->bucket[cache_id].itemp_index;
    if(res.itemp_index ==-1)
        fprintf(stderr, "error: itemp_index == -1\n");
    // lsassert(res.itemp_index != -1);
    //=======cache setting after allocate======
    imm_cache->curr_imm_index++;
    imm_cache->curr_opt_cache_id = cache_id;
    imm_cache->optimized_ir2 = true;

#ifdef LATXS_IMM_CACHE_DEBUG
    imm_log("After Allocate:\n");
    imm_cache_print_cache();
    imm_log("-------\n");
    imm_log("[Result] diff=%lx\toffset=%lx\tcached:%d\n", res.diff, res.offset, res.cached);
    imm_log("-------\n");
#endif
    return res;
}
/**
 * succeed imm will fetch the first cache value
 * avg_offset indicates whether the offset is pre cached in ahead
 * if avg_offset, should not update
 * else update offset, avg_offset should always be 0;
 * return diff;
 */
long imm_cache_diff(int cache_id, int64 new_offset)
{

    IMM_CACHE_BUCKET *bucket = &(imm_cache->bucket[cache_id]);
    int64 criterion_offset = bucket->avg_offset == 0 ? bucket->offset : bucket->avg_offset;

    /* hit */
    long diff = (long)(new_offset - criterion_offset);
    // if (bucket->avg_offset == 0) {
    //     diff = new_offset - bucket->offset;
    // } else {
    //     diff = new_offset - bucket->avg_offset;
    // }
    return diff;
}

void imm_cache_update_by_diff(int cache_id, long diff)
{
    int64 old_offset = imm_cache->bucket[cache_id].offset;
    imm_log("[update by diff] 0x%lx + 0x%lx = 0x%lx\n", old_offset, diff, old_offset + diff);
    imm_cache->bucket[cache_id].offset += diff;
}

// void imm_cache_update_by_offset(int cache_id, int64 new_offset)
// {
//     imm_log("[update by offset]new:%lx\n", new_offset);
//     imm_cache->bucket[cache_id].offset = new_offset;
// }

void imm_cache_addi_update(long mem_offset)
{
    if (can_do_imm() && (option_imm_rip || option_imm_pattern) &&
        mem_offset != 0) {
        IMM_CACHE *imm_cache = lsenv->tr_data->imm_cache;
        if (imm_cache->opt_take_effect) {
            imm_cache_update_by_diff(imm_cache->curr_opt_cache_id, mem_offset);
        }
    }
}
/**
 * called during translation period(not ir1 precache)
 * 1. a new cache( include 'new' and 'miss' )
 * 2. never changes avg_offset
 * 3. find a free cache, if none, free the least used one
 */
int imm_cache_put(int base, int index, int scale, int64 offset)
{
    int cache_id = -1;
    if (imm_cache->free_count == 0) {
        /*replace the least used cache */
        cache_id = imm_cache->cache_count - 1;
        imm_log("[replace]%d\n", cache_id);
        imm_cache_free(cache_id);
        imm_cache->free_count++;
    } else {
        cache_id = imm_cache->cache_count - imm_cache->free_count;
        imm_log("[use]%d\t\n", cache_id);
    }

    imm_cache_fill_bucket(cache_id, base, index, scale, offset);
    imm_cache->free_count--;

    imm_cache->bucket[cache_id].curr_ir1_index = imm_cache->curr_ir1_index;
    imm_cache->bucket[cache_id].use_count++;
    imm_cache->bucket[cache_id].itemp_index = alloc_cache_itemp();
    return cache_id;
}

void imm_cache_free_itemp(int itemp_index)
{
    for (int i = 0; i < imm_cache->cache_count; i++) {
        if (imm_cache->bucket[i].itemp_index == itemp_index && !imm_cache->bucket[i].free) {
            imm_cache_free(i);
            imm_cache->free_count++;
        }
    }
}

/**
 * we need clean the cache during precache put
 * */
void imm_cache_free_dead_cache(void)
{
    imm_log("[allocate][free dead]");
    for (int i = 0; i < imm_cache->cache_count; i++) {
        int life = imm_cache->bucket[i].lifecycle;
        if (!imm_cache->bucket[i].free && life != 0 && life < imm_cache->curr_imm_index) {
            imm_log("%d\t", i);
            imm_cache_free(i);
            imm_cache->free_count++;
        }
    }

    imm_log("\n");
    imm_cache_sort();
    imm_cache_print_cache();
}

void imm_cache_free(int id)
{
    free_cache_itemp(imm_cache->bucket[id].itemp_index);
    imm_cache_init_bucket(imm_cache->bucket, id);
}

void imm_cache_free_all(void)
{
    if (imm_cache == NULL) {
        return;
    }
    for (int i = 0; i < imm_cache->cache_count; i++) {
        if (!imm_cache->bucket[i].free) {
            imm_cache_free(i);
        }
    }
    imm_cache->free_count = imm_cache->cache_count;
    imm_cache->itemp_allocated = false;
    imm_cache->opt_take_effect = false;
    imm_cache->curr_opt_cache_id = -1;
}

void imm_cache_print_bucket(IMM_CACHE_BUCKET *bucket, int index)
{
    IMM_CACHE_BUCKET b = bucket[index];
    char imm_str[50];
    imm_to_str(imm_str, b.base, b.index, b.scale, b.offset);
    imm_log("[IMM CACHE] BUCKET[%d] allocate [ %s ] at itemp%d, use:%d, ir1_index:%d\n",
            index,
            imm_str,
            b.itemp_index,
            b.use_count,
            b.curr_ir1_index);
}

void imm_cache_print_cache(void)
{
    if (!option_imm_debug) {
        return;
    }
    imm_log("[IMM CACHE] Usage: {");
    int f = 0;
    for (int i = 0; i < 4; i++) {
        if (cache_itemp_use_stat[i] == 1) {
            f++;
        }
        imm_log(" %d", cache_itemp_use_stat[i]);
    }
    if (f != imm_cache->free_count) {
        printf("free count doesn't equal.\n");
    }
    imm_log(" } %d/%d \timm_index:%d\tir1_index:%d\n",
            imm_cache->free_count,
            imm_cache->cache_count,
            imm_cache->curr_imm_index,
            imm_cache->curr_ir1_index);
    for (int i = 0; i < imm_cache->cache_count; i++) {
        if (!imm_cache->bucket[i].free) {
            imm_cache_print_bucket(imm_cache->bucket, i);
        }
    }
}

void imm_cache_print_ir2(IR2_INST *ir2, int index)
{
    char str[64];
    if (!option_imm_debug || ir2_opcode(ir2) == 0) {
        return;
    }
    latxs_ir2_to_string(ir2, str);

    if (str[0] == '-') {
        imm_log("[%d]%s\n", index, str);
    } else {
        imm_log("[%d]%s\n", index, str);
    }
}

void imm_cache_print_ir1(IR1_INST *pir1)
{
    imm_log("[IR1] 0x%" PRIx64 ": %s\t\t%s\n",
            pir1->info->address,
            pir1->info->mnemonic,
            pir1->info->op_str);
}

void imm_cache_print_tb_ir1(TranslationBlock *tb)
{
#ifdef LATX_DISASSEMBLE_TRACE_DEBUG
    IR1_INST *pir1 = tb_ir1_inst(tb, 0);
    for (int i = 0; i < tb->icount; ++i) {
        imm_log("IR1[%d]\t: %s\t%s\n", i, pir1->info->mnemonic, pir1->info->op_str);
        pir1++;
    }
#endif
}

void imm_cache_print_tr_ir2(void)
{
    if (!option_imm_debug) {
        return;
    }

    TRANSLATION_DATA *t = lsenv->tr_data;
    for (int i = imm_cache->curr_ir2_index; i < t->ir2_cur_nr; i++) {
        IR2_INST *pir2 = &t->ir2_array[i];
        imm_cache_print_ir2(pir2, i);
    }
}

//
void trace_file_init(void)
{
    if (unlikely(option_imm_debug)) {
        if (unlikely(trace_file == NULL)) {
            trace_file = imm_open_debug_file("trace");
        }
    }
}
void latxs_imm_init(TranslationBlock *tb)
{
    if (unlikely(option_imm_debug)) {
        if (unlikely(log_file == NULL)) {
            log_file = imm_open_debug_file("cache");
        }
    }

    /* first init imm cache */
    if (unlikely(imm_cache == NULL)) {
        imm_cache = (IMM_CACHE *)mm_malloc(sizeof(IMM_CACHE));
        // TODO
        // init bucket
        imm_cache->bucket =
            (IMM_CACHE_BUCKET *)mm_calloc(CACHE_MAX_CAPACITY, sizeof(IMM_CACHE_BUCKET));

        lsenv->tr_data->imm_cache = imm_cache;
    }
    imm_cache->curr_pc = tb->pc;
    imm_cache_init(CACHE_DEFAULT_CAPACITY);
    return;
}

void imm_log_tb_pc(uint64_t pc)
{
    if (likely(!option_imm_debug))
        return;
    if (unlikely(log_pc_file == NULL)) {
        log_pc_file = imm_open_debug_file("pc");
    }
    if (likely(log_pc_file != NULL)) {
        fprintf(log_pc_file, "0x%lx\n", pc);
    }
}
bool dynamic_option_imm_cache(void) { return lsenv->tr_data->dynamic_option_imm; }
void imm_cache_set_dynamic_option(bool option_value, const char *message)
{
    if (option_value) {
        imm_trace("imm dynamic option on. %s.\n", message);
    }
    lsenv->tr_data->dynamic_option_imm = option_value;
}
bool can_do_imm(void)
{
    if (option_imm_mode == 1) {
        //dynamic
        return dynamic_option_imm_cache();
    } else {
        //static
        return option_imm_cache;
    }
}
