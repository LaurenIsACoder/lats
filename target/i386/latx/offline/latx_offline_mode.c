/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * latx_offline_mode.c - Offline X86 to LoongArch translation mode for QEMU
 *
 * This implements offline X86 to LoongArch translation within QEMU process.
 * Usage: qemu-x86_64 -offline [options] <x86_hex_bytes>
 *
 * Based on target_latxs_host implementation.
 * Uses mmap to create a memory region for code fetching.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "latx_offline_mode.h"
#include "latx/include/lsenv.h"
#include "latx/include/env.h"
#include "latx/include/ir1.h"
#include "latx/include/ir2.h"
#include "latx/include/translate.h"
#include "latx/include/latx-options.h"
#include "latx/include/reg-alloc.h"
#include "latx/include/latx-sys-env-init-func.h"
#include "latx/include/latx-static-codes.h"
#include "capstone/capstone.h"
#include <getopt.h>
#include <ctype.h>
#include <sys/mman.h>

/* Global flag to indicate offline mode */
int latx_offline_mode_active = 0;

/* Maximum sizes */
#define MAX_OFFLINE_CODE_SIZE 4096
#define MAX_OFFLINE_INSNS     128
#define OFFLINE_STATIC_CODE_SIZE (64 * 1024)  /* 64KB for static codes */

/* Offline translation context with proper embedding for env_cpu() to work */
typedef struct OfflineTranslateContext {
    X86CPU x86_cpu;         /* Must be first - contains CPUState then CPUX86State */
    TranslationBlock tb;
    struct separated_data s_data;  /* For CONFIG_LATX_TU */
} OfflineTranslateContext;

static OfflineTranslateContext offline_ctx;

/* Code generation buffer for offline mode */

/* Static code buffer for offline mode */
static uint8_t *offline_static_code_buffer = NULL;

/* mmap'd memory for code */
static void *offline_code_mem = NULL;
static target_ulong offline_code_base = 0x40000000;

/* Static code table for offline mode */
static latx_sc_table_t offline_sc_table;

static void print_usage(const char *prog)
{
    fprintf(stderr, "LATX Offline X86 to LoongArch Translator\n");
    fprintf(stderr, "Usage: %s -offline [options] <x86_hex_bytes>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -m, --mode <16|32|64>  X86 mode (default: 32)\n");
    fprintf(stderr, "  -p, --pc <address>     Starting PC in hex (default: 0x40000000)\n");
    fprintf(stderr, "  -n, --num <count>      Max instructions to translate (default: 16)\n");
    fprintf(stderr, "  -h, --help             Show this help\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s -offline -m 32 \"b8 01 00 00 00\"   # MOV EAX, 1\n", prog);
    fprintf(stderr, "  %s -offline -m 64 \"48 89 c3\"         # MOV RBX, RAX\n", prog);
}

static int parse_hex(const char *str, uint8_t *buf, size_t max_size)
{
    size_t len = 0;
    const char *p = str;

    while (*p && len < max_size) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ',' || *p == ':' || *p == '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        int val;
        if (sscanf(p, "%2x", &val) != 1) {
            return -1;
        }
        buf[len++] = (uint8_t)val;
        p += (isxdigit(p[0]) && isxdigit(p[1])) ? 2 : 1;
    }
    return len;
}

/* Setup mmap'd memory for code */
static int setup_code_memory(uint8_t *code, size_t size)
{
    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t map_size = (size + page_size - 1) & ~(page_size - 1);
    map_size = map_size < page_size ? page_size : map_size;

    offline_code_mem = mmap((void *)(uintptr_t)offline_code_base, map_size,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                            -1, 0);

    if (offline_code_mem == MAP_FAILED) {
        offline_code_mem = mmap(NULL, map_size,
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS,
                               -1, 0);
        if (offline_code_mem == MAP_FAILED) {
            fprintf(stderr, "Error: mmap failed for code memory\n");
            return -1;
        }
        offline_code_base = (target_ulong)(uintptr_t)offline_code_mem;
        fprintf(stderr, "Note: Code mapped at 0x%lx\n", (unsigned long)offline_code_base);
    }

    memcpy(offline_code_mem, code, size);
    return 0;
}

/* Initialize static code buffer for jumps */
static int init_static_codes(void)
{
    /* Allocate static code buffer */
    offline_static_code_buffer = mmap(NULL, OFFLINE_STATIC_CODE_SIZE,
                                      PROT_READ | PROT_WRITE | PROT_EXEC,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);
    if (offline_static_code_buffer == MAP_FAILED) {
        fprintf(stderr, "Error: mmap failed for static code buffer\n");
        return -1;
    }

    /* Point latx_sc_table to our local table */
    latx_sc_table = &offline_sc_table;
    memset(&offline_sc_table, 0, sizeof(latx_sc_table_t));

    /* Generate minimal static codes for context switch */
    uintptr_t code_addr = (uintptr_t)offline_static_code_buffer;

    /* Set up minimal context switch return points */
    SET_SC_TABLE(0, cs_native_to_bt_ret_0, code_addr);
    SET_SC_TABLE(0, cs_native_to_bt_ret_n0, code_addr + 8);
    SET_SC_TABLE(0, cs_native_to_bt_ret_n1, code_addr + 16);
    SET_SC_TABLE(0, cs_native_to_bt_ret_n3, code_addr + 24);

    /* Simple return code */
    uint32_t *code_ptr = (uint32_t *)code_addr;

    /* ret_0: or , , ; b .+12 */
    code_ptr[0] = 0x00150000;  /* or , ,  */
    code_ptr[1] = 0x50000c00;  /* b 3 */

    /* ret_n3: ori , , 3; b .+8 */
    code_ptr[2] = 0x03800804;  /* ori , , 3 */
    code_ptr[3] = 0x50000800;  /* b 2 */

    /* ret_n1: ori , , 1; b .+4 */
    code_ptr[4] = 0x03800404;  /* ori , , 1 */
    code_ptr[5] = 0x50000400;  /* b 1 */

    /* ret_n0: ori , , 0; jirl , , 0 */
    code_ptr[6] = 0x03800004;  /* ori , , 0 */
    code_ptr[7] = 0x4c000020;  /* jirl , , 0 */

    /* prologue entry point */
    SET_SC_TABLE(0, cs_bt_to_native, code_addr + 32);

    /* Disable AOT save to avoid reset_region_offset issues */
    option_aot_save = 0;
    option_sys_aot = 0;
    option_large_code_cache = 0;
    option_tb_link = 0;

    return 0;
}

/* Initialize offline translation context */
static void init_offline_context(OfflineTranslateContext *ctx, int mode, target_ulong pc)
{
    memset(ctx, 0, sizeof(OfflineTranslateContext));

    X86CPU *x86_cpu = &ctx->x86_cpu;
    CPUState *cpu = CPU(x86_cpu);
    CPUX86State *env = &x86_cpu->env;
    TranslationBlock *tb = &ctx->tb;
    struct separated_data *s_data = &ctx->s_data;

    /* Initialize QOM object basics for type checking */
    Object *obj = OBJECT(x86_cpu);
    obj->class = object_class_by_name("x86_64-cpu");
    if (!obj->class) {
        obj->class = object_class_by_name("x86-cpu");
    }
    obj->ref = 1;

    /* Initialize CPUState */
    cpu->cpu_index = 0;
    cpu->tcg_cflags = CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
    cpu->nr_cores = 1;
    cpu->nr_threads = 1;
    cpu->env_ptr = env;

    /* Initialize CPUX86State */
    if (mode == 64) {
        env->hflags |= HF_LMA_MASK | HF_CS64_MASK;
    } else if (mode == 32) {
        env->hflags |= HF_CS32_MASK;
    }
    env->eip = pc;

    /* Set CPU features */
    env->features[FEAT_1_EDX] = CPUID_CX8 | CPUID_CMOV | CPUID_SSE | CPUID_SSE2;
    env->features[FEAT_1_ECX] = CPUID_EXT_SSE3 | CPUID_EXT_SSSE3 | CPUID_EXT_SSE41 | CPUID_EXT_SSE42 | CPUID_EXT_POPCNT;
    env->features[FEAT_7_0_EBX] = 0;

    /* Initialize TranslationBlock */
    tb->pc = pc;
    tb->cs_base = 0;
    tb->flags = env->hflags;
    tb->cflags = CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
    tb->size = 0;
    tb->icount = 0;

    /* Set up code generation buffer - place close to static codes */
    tb->tc.ptr = (tcg_insn_unit *)(offline_static_code_buffer + 4096);

    /* Link s_data to tb */
    tb->s_data = s_data;
    s_data->tu_tb_mode = TB_GEN_CODE;

    /* Initialize lsenv */
    latx_lsenv_init(env);
}

/* Main offline translation function */
int latx_offline_translate(const char *hex_bytes, int mode, uint64_t pc_arg, int max_insns)
{
    uint8_t code[MAX_OFFLINE_CODE_SIZE];
    int size = parse_hex(hex_bytes, code, sizeof(code));

    if (size <= 0) {
        fprintf(stderr, "Error: Invalid hex input\n");
        return 1;
    }

    if (pc_arg != 0) {
        offline_code_base = (target_ulong)pc_arg;
    }

    fprintf(stderr, "LATX Offline X86 to LoongArch Translator\n");
    fprintf(stderr, "========================================\n\n");
    fprintf(stderr, "Input: %d bytes, mode=%d, pc=0x%lx, max_insns=%d\n\n",
            size, mode, (unsigned long)offline_code_base, max_insns);

    /* Initialize static codes for jump instructions */
    if (init_static_codes() < 0) {
        return 1;
    }

    /* Setup mmap'd memory for code */
    if (setup_code_memory(code, size) < 0) {
        return 1;
    }

    target_ulong pc = offline_code_base;

    /* Initialize context */
    init_offline_context(&offline_ctx, mode, pc);

    TranslationBlock *tb = &offline_ctx.tb;
    TRANSLATION_DATA *td = lsenv->tr_data;

    /* Initialize translation system */
    latxs_tr_sys_init(tb, max_insns, NULL, NULL);
    td->is_tu_mode = 0;

    /* Disassemble X86 to IR1 */
    latxs_tr_disasm(tb);

    fprintf(stderr, "Disassembled %d X86 instruction(s)\n", td->ir1_nr);

    /* Print IR1 instructions */
    fprintf(stderr, "\nX86 Instructions:\n");
    for (int i = 0; i < td->ir1_nr; i++) {
        IR1_INST *ir1 = &td->ir1_inst_array[i];
        if (ir1->info) {
            fprintf(stderr, "  %08" PRIx64 ": ", (uint64_t)ir1_addr(ir1));
            for (int j = 0; j < ir1->info->size; j++) {
                fprintf(stderr, "%02x ", ir1->info->bytes[j]);
            }
            fprintf(stderr, "  %-8s %s\n", ir1->info->mnemonic, ir1->info->op_str);
        }
    }

    /* Initialize IR2 generation */
    latxs_tr_init(tb);

    /* Generate TB start */
    latxs_tr_gen_tb_start();

    /* Translate each IR1 instruction */
    for (int i = 0; i < td->ir1_nr; i++) {
        latxs_tr_init_translate_ir1(tb, i);

        IR1_INST *ir1 = &td->ir1_inst_array[i];
        bool result = ir1_translate(ir1);
        if (!result) {
            fprintf(stderr, "Warning: Translation failed for instruction %d\n", i);
        }
    }

    /* Generate TB end */
    latxs_tr_gen_tb_end();

    fprintf(stderr, "\nGenerated %d LoongArch instruction(s)\n\n", td->ir2_cur_nr);

    /* Print IR2 instructions */
    printf("LoongArch Instructions:\n");
    char buf[256];
    for (int i = 0; i < td->ir2_cur_nr; i++) {
        latxs_ir2_to_string(&td->ir2_array[i], buf);
        uint32_t mcode = ir2_assemble(&td->ir2_array[i]);
        printf("  %-40s  ; 0x%08x\n", buf, mcode);
    }

    /* Cleanup */
    latxs_tr_fini();

    /* Unmap code memory */
    if (offline_code_mem && offline_code_mem != MAP_FAILED) {
        size_t page_size = sysconf(_SC_PAGESIZE);
        size_t map_size = (size + page_size - 1) & ~(page_size - 1);
        munmap(offline_code_mem, map_size);
        offline_code_mem = NULL;
    }

    /* Unmap static code buffer */
    if (offline_static_code_buffer && offline_static_code_buffer != MAP_FAILED) {
        munmap(offline_static_code_buffer, OFFLINE_STATIC_CODE_SIZE);
        offline_static_code_buffer = NULL;
    }

    return 0;
}

int check_offline_mode(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-offline") == 0) {
            return i;
        }
    }
    return 0;
}

int run_offline_mode(int argc, char **argv, int offline_idx)
{
    int mode = 32;
    uint64_t pc = 0;
    int max_insns = 16;
    const char *hex_bytes = NULL;

    latx_offline_mode_active = 1;


    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"pc",   required_argument, 0, 'p'},
        {"num",  required_argument, 0, 'n'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    optind = offline_idx + 1;

    int c;
    while ((c = getopt_long(argc, argv, "m:p:n:h", opts, NULL)) != -1) {
        switch (c) {
        case 'm':
            mode = atoi(optarg);
            if (mode != 16 && mode != 32 && mode != 64) {
                fprintf(stderr, "Error: Invalid mode %s\n", optarg);
                exit(1);
            }
            break;
        case 'p':
            pc = strtoull(optarg, NULL, 16);
            break;
        case 'n':
            max_insns = atoi(optarg);
            if (max_insns < 1) max_insns = 1;
            if (max_insns > MAX_OFFLINE_INSNS) max_insns = MAX_OFFLINE_INSNS;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No hex bytes provided\n");
        print_usage(argv[0]);
        exit(1);
    }

    hex_bytes = argv[optind];

    module_call_init(MODULE_INIT_QOM);
    qemu_init_cpu_list();

    return latx_offline_translate(hex_bytes, mode, pc, max_insns);
}
