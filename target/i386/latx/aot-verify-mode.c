/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "sys-aot.h"
#include "sbt/sys-aot-internal.h"
#include "../tb-record.h"
#include "aot-verify-mode.h"
#include "latx-options.h"
#include <getopt.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_MISMATCHES    4096

static void *load_aot_file_unsafe(const char *file_name, uint64_t *size) {
    struct stat sb;
    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "open %s fail\n", file_name);
        return NULL;
    }
    if (fstat(fd, &sb) == -1) {
        fprintf(stderr, "fstat %s fail\n", file_name);
        close(fd);
        return NULL;
    }
    aot_verbose_log("[AOT] %s size %ldMB\n", file_name, sb.st_size/1000000);
    *size = sb.st_size;
    void *mem_ptr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem_ptr == MAP_FAILED) {
        fprintf(stderr, "mmap errno=%d, err_msg=\"%s\"\n", errno, strerror(errno));
        close(fd);
        return NULL;
    }
    close(fd);
    if (!validate_aot_layout(mem_ptr, sb.st_size)) {
        fprintf(stderr, "invalid AOT layout in %s\n", file_name);
        munmap(mem_ptr, sb.st_size);
        return NULL;
    }
    return mem_ptr;
}

typedef struct {
    uint64_t pc;
    uint32_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint32_t ir2_num;
    uint32_t ir2_len;
    uint8_t *ir2_data;
    uint8_t *guest_code[2];
    uint32_t rel_count;
    uint8_t *rel_data;
    int matched;
} aot_tb_entry;

typedef struct {
    uint64_t pc;
    uint32_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint32_t rec_ir2_len;
    uint32_t aot_ir2_len;
    uint8_t *rec_ir2;
    uint8_t *aot_ir2;
    uint32_t rec_ir1_len;
    uint8_t *rec_ir1;
    uint8_t *aot_guest_code[2];
    int mismatch_type;
} mismatch_info;

typedef struct {
    uint64_t pc;
    uint32_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint32_t ir1_len;
    uint32_t ir2_len;
    uint8_t *ir1_data;
    uint8_t *ir2_data;
} tb_rec;

static aot_tb_entry *g_aot_tbs;
static int g_aot_tb_count;
static int g_aot_tb_capacity;
static mismatch_info g_mismatches[MAX_MISMATCHES];
static int g_mismatch_count;
static int g_total_mismatch_count;
static int g_unmatched_rec;
static int g_unmatched_aot;
static int g_total_matched;
static int g_rec_count = 0;

typedef struct { uint16_t op15; const char *name; } ir2_op;
static const ir2_op ir2_ops[] = {
    {0x020,"add.w"},{0x021,"add.d"},{0x022,"sub.w"},{0x023,"sub.d"},
    {0x024,"slt"},{0x025,"sltu"},{0x028,"nor"},{0x029,"and"},
    {0x02a,"or"},{0x02b,"xor"},{0x02e,"sll.w"},{0x02f,"sll.d"},
    {0x030,"srl.w"},{0x031,"srl.d"},{0x032,"sra.w"},{0x033,"sra.d"},
    {0x038,"mul.w"},{0x039,"mulh.w"},{0x03b,"mul.d"},
    {0x040,"div.w"},{0x041,"mod.w"},{0x044,"div.d"},{0x045,"mod.d"},
    {0x048,"break"},{0x04a,"syscall"},
    {0x088,"alsl.w"},{0x08a,"alsl.d"},
    {0x0a8,"slti"},{0x0a9,"sltui"},{0x0aa,"addi.w"},{0x0ab,"addi.d"},
    {0x0ad,"andi"},{0x0ae,"ori"},{0x0af,"xori"},
    {0x0b8,"slli.w"},{0x0b9,"slli.d"},{0x0ba,"srli.w"},{0x0bb,"srli.d"},
    {0x0bc,"srai.w"},{0x0bd,"srai.d"},
    {0x140,"ld.b"},{0x141,"ld.h"},{0x142,"ld.w"},{0x143,"ld.d"},
    {0x144,"st.b"},{0x145,"st.h"},{0x146,"st.w"},{0x147,"st.d"},
    {0x148,"ld.bu"},{0x149,"ld.hu"},{0x14a,"ld.wu"},
    {0x150,"fld.s"},{0x151,"fst.s"},{0x152,"fld.d"},{0x153,"fst.d"},
    {0x160,"beq"},{0x161,"bne"},{0x162,"blt"},{0x163,"bge"},
    {0x164,"bltu"},{0x165,"bgeu"},{0x166,"beqz"},{0x167,"bnez"},
    {0x180,"lu12i.w"},{0x181,"lu32i.d"},{0x183,"pcalau12i"},
    {0x184,"pcaddu12i"},{0x1c0,"jirl"},
    {0x2c0,"dbar"},{0x2c1,"ibar"},
    {0, NULL}
};

static const char *ir2_mnemonic(uint32_t inst) {
    uint16_t op = (uint16_t)(inst >> 15);
    for (const ir2_op *e = ir2_ops; e->name; e++)
        if (e->op15 == op) return e->name;
    return NULL;
}

static int ir2_disasm(char *buf, int sz, uint32_t inst) {
    uint8_t rd = (inst >> 0) & 0x1f, rj = (inst >> 5) & 0x1f, rk = (inst >> 10) & 0x1f;
    int32_t si12 = (int32_t)(inst >> 10) & 0xfff;
    if (si12 & 0x800) si12 |= ~0xfff;
    int32_t si16 = (int32_t)(inst >> 10) & 0xffff;
    if (si16 & 0x8000) si16 |= ~0xffff;
    int32_t si20 = (int32_t)(inst >> 5) & 0xfffff;
    if (si20 & 0x80000) si20 |= ~0xfffff;
    const char *mn = ir2_mnemonic(inst);
    if (!mn) return snprintf(buf, sz, "%08x", inst);
    if (strcmp(mn, "jirl") == 0)
        return snprintf(buf, sz, "%-8s r%d, r%d, %d", mn, rd, rj, si16);
    if (strcmp(mn, "beq") == 0 || strcmp(mn, "bne") == 0 || strcmp(mn, "blt") == 0 ||
        strcmp(mn, "bge") == 0 || strcmp(mn, "bltu") == 0 || strcmp(mn, "bgeu") == 0)
        return snprintf(buf, sz, "%-8s r%d, r%d, %d", mn, rd, rj, si16);
    if (strcmp(mn, "beqz") == 0 || strcmp(mn, "bnez") == 0)
        return snprintf(buf, sz, "%-8s r%d, %d", mn, rj, si16);
    if (strcmp(mn, "lu12i.w") == 0 || strcmp(mn, "lu32i.d") == 0 ||
        strcmp(mn, "pcalau12i") == 0 || strcmp(mn, "pcaddu12i") == 0)
        return snprintf(buf, sz, "%-8s r%d, %d", mn, rd, si20);
    if (strncmp(mn, "ld.", 3) == 0 || strncmp(mn, "st.", 3) == 0 ||
        strncmp(mn, "fld.", 4) == 0 || strncmp(mn, "fst.", 4) == 0 ||
        strncmp(mn, "ldbu", 4) == 0 || strncmp(mn, "ldhu", 4) == 0 ||
        strncmp(mn, "ldwu", 4) == 0)
        return snprintf(buf, sz, "%-8s r%d, r%d, %d", mn, rd, rj, si12);
    if (strcmp(mn, "slti") == 0 || strcmp(mn, "sltui") == 0 ||
        strcmp(mn, "addi.w") == 0 || strcmp(mn, "addi.d") == 0 ||
        strcmp(mn, "andi") == 0 || strcmp(mn, "ori") == 0 || strcmp(mn, "xori") == 0)
        return snprintf(buf, sz, "%-8s r%d, r%d, %d", mn, rd, rj, si12);
    if (strcmp(mn, "slli.w") == 0 || strcmp(mn, "slli.d") == 0 ||
        strcmp(mn, "srli.w") == 0 || strcmp(mn, "srli.d") == 0 ||
        strcmp(mn, "srai.w") == 0 || strcmp(mn, "srai.d") == 0) {
        uint8_t ui5 = (inst >> 10) & 0x1f;
        return snprintf(buf, sz, "%-8s r%d, r%d, %u", mn, rd, rj, ui5);
    }
    return snprintf(buf, sz, "%-8s r%d, r%d, r%d", mn, rd, rj, rk);
}

static void ir2_side_by_side(FILE *f, uint8_t *rec, uint32_t rec_len,
                              uint8_t *aot, uint32_t aot_len) {
    uint32_t rc = rec_len / 4, ac = aot_len / 4, mc = rc > ac ? rc : ac;
    fprintf(f, "      %-4s  %-40s | %-4s  %-40s\n", "REC#", "RECORD", "AOT#", "AOT");
    fprintf(f, "      %-4s  %-40s | %-4s  %-40s\n",
            "----", "----------------------------------------",
            "----", "----------------------------------------");
    for (uint32_t i = 0; i < mc; i++) {
        char l[80] = "", r[80] = "";
        if (i < rc) ir2_disasm(l, 80, *(uint32_t *)(rec + i * 4));
        if (i < ac) ir2_disasm(r, 80, *(uint32_t *)(aot + i * 4));
        int diff = (i < rc && i < ac) ? memcmp(rec + i * 4, aot + i * 4, 4) : 1;
        if (i < rc && i < ac)
            fprintf(f, "  %s %-4u  %-40s | %-4u  %-40s\n", diff ? "!=" : "  ", i, l, i, r);
        else if (i < rc)
            fprintf(f, "  != %-4u  %-40s | %-4s  %-40s\n", i, l, "--", "(absent)");
        else
            fprintf(f, "  != %-4s  %-40s | %-4u  %-40s\n", "--", "(absent)", i, r);
    }
}

static int parse_tb_record_file(const char *path, tb_rec **out_recs, int *out_count) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Error: cannot open '%s'\n", path); return -1; }
    if (fseeko(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    off_t file_size = ftello(fp);
    if (file_size < 16 || fseeko(fp, 0, SEEK_SET) != 0) {
        fclose(fp); return -1;
    }
    uint32_t hdr[4];
    if (fread(hdr, 16, 1, fp) != 1) { fclose(fp); return -1; }
    if (hdr[0] != TB_RECORD_MAGIC) {
        fprintf(stderr, "Error: bad magic 0x%08x\n", hdr[0]);
        fclose(fp); return -1;
    }
    if (hdr[2] > INT_MAX ||
            hdr[2] > ((uint64_t)file_size - 16) / sizeof(TBRecordHeader)) {
        fprintf(stderr, "Error: invalid TB record count %u\n", hdr[2]);
        fclose(fp); return -1;
    }
    int count = hdr[2];
    tb_rec *recs = calloc(count, sizeof(tb_rec));
    if (!recs) { fclose(fp); return -1; }
    for (int i = 0; i < count; i++) {
        TBRecordHeader rh;
        if (fread(&rh, sizeof(rh), 1, fp) != 1) {
            goto fail;
        }
        recs[i].pc = rh.pc;
        recs[i].cs_base = rh.cs_base;
        recs[i].flags = rh.flags;
        recs[i].cflags = rh.cflags;
        recs[i].ir1_len = rh.ir1_len;
        recs[i].ir2_len = rh.ir2_len;
        off_t data_pos = ftello(fp);
        uint64_t data_len = (uint64_t)rh.ir1_len + rh.ir2_len;
        if (data_pos < 0 || data_len > (uint64_t)file_size - data_pos) {
            goto fail;
        }
        if (rh.ir1_len > 0) {
            recs[i].ir1_data = malloc(rh.ir1_len);
            if (!recs[i].ir1_data ||
                    fread(recs[i].ir1_data, 1, rh.ir1_len, fp) != rh.ir1_len) {
                goto fail;
            }
        }
        if (rh.ir2_len > 0) {
            recs[i].ir2_data = malloc(rh.ir2_len);
            if (!recs[i].ir2_data ||
                    fread(recs[i].ir2_data, 1, rh.ir2_len, fp) != rh.ir2_len) {
                goto fail;
            }
        }
    }
    fclose(fp);
    *out_recs = recs;
    *out_count = count;
    return 0;

fail:
    fclose(fp);
    for (int i = 0; i < count; i++) {
        free(recs[i].ir1_data);
        free(recs[i].ir2_data);
    }
    free(recs);
    return -1;
}

static aot_tb_entry *append_aot_tb(void)
{
    if (g_aot_tb_count == g_aot_tb_capacity) {
        int new_capacity = g_aot_tb_capacity ? g_aot_tb_capacity * 2 : 65536;
        aot_tb_entry *entries;

        if (new_capacity < g_aot_tb_capacity) {
            return NULL;
        }
        entries = g_try_realloc_n(g_aot_tbs, new_capacity, sizeof(*entries));
        if (!entries) {
            return NULL;
        }
        g_aot_tbs = entries;
        g_aot_tb_capacity = new_capacity;
    }
    return &g_aot_tbs[g_aot_tb_count++];
}

static GHashTable *build_aot_guest_page_index(uint8_t *data,
                                              uint64_t data_size)
{
    const aot_tail *tail = (const aot_tail *)(data + data_size -
            sizeof(*tail));
    size_t page_info_size;
    const page_info_array_t *info_array;
    GHashTable *pages;

    if (tail->page_num < 0 ||
            !aot_page_info_size(tail->page_num, &page_info_size)) {
        return NULL;
    }
    info_array = (const page_info_array_t *)((const uint8_t *)tail -
            page_info_size);
    pages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (uint32_t i = 0; i < info_array->page_count; i++) {
        uint8_t *guest_code = data + info_array->pages[i].file_offset +
                sizeof(page_head);
        uint64_t fingerprint = get_page_fingerprint(guest_code);

        if (!g_hash_table_contains(pages, &fingerprint)) {
            g_hash_table_insert(pages,
                    g_memdup2(&fingerprint, sizeof(fingerprint)), guest_code);
        }
    }
    return pages;
}

static uint8_t *find_aot_tu_guest_page(GHashTable *guest_pages,
                                       const aot_tu_page *pages,
                                       uint32_t page_count,
                                       int64_t pc_delta)
{
    for (uint32_t i = 0; i < page_count; i++) {
        if (pages[i].pc_delta == pc_delta) {
            return g_hash_table_lookup(guest_pages,
                                       &pages[i].page_fingerprint);
        }
    }
    return NULL;
}

static bool aot_ir1_matches(const aot_tb_entry *aot, const tb_rec *record)
{
    uint32_t offset = record->pc & (TARGET_PAGE_SIZE - 1);
    uint32_t first_size = MIN(record->ir1_len,
                              TARGET_PAGE_SIZE - offset);
    uint32_t second_size = record->ir1_len - first_size;

    if (!aot->guest_code[0] ||
            memcmp(record->ir1_data, aot->guest_code[0] + offset,
                   first_size) != 0) {
        return false;
    }
    return second_size == 0 ||
        (second_size <= TARGET_PAGE_SIZE && aot->guest_code[1] &&
         memcmp(record->ir1_data + first_size, aot->guest_code[1],
                second_size) == 0);
}

static uint8_t aot_ir1_byte(const mismatch_info *mismatch, uint32_t index)
{
    uint32_t offset = (mismatch->pc & (TARGET_PAGE_SIZE - 1)) + index;
    uint32_t page = offset / TARGET_PAGE_SIZE;

    if (page >= ARRAY_SIZE(mismatch->aot_guest_code) ||
            !mismatch->aot_guest_code[page]) {
        return 0;
    }
    return mismatch->aot_guest_code[page][offset &
            (TARGET_PAGE_SIZE - 1)];
}

static int parse_aot_data(uint8_t *data, uint64_t data_size) {
    const aot_tail *tail = (const aot_tail *)(data + data_size - sizeof(*tail));
    bool tu_format = (tail->flags &
            (UINT64_C(1) << AOT_TU_FLAG_BIT)) != 0;
    GHashTable *guest_pages = tu_format ?
        build_aot_guest_page_index(data, data_size) : NULL;
    uint64_t offset = 0;
    if (tu_format && !guest_pages) {
        return -1;
    }
    while (offset + sizeof(page_head) <= data_size) {
        page_head *ph = (page_head *)(data + offset);
        if (ph->page_magic != AOT_PAGE_MAGIC && ph->page_magic != AOT_MERGED_MAGIC)
            break;
        if (ph->curr_page_sum_size <= 0 || offset + ph->curr_page_sum_size > data_size)
            break;
        uint8_t *guest_code = data + offset + sizeof(page_head);
        uint64_t tb_off = offset + sizeof(page_head) + TARGET_PAGE_SIZE;
        uint64_t page_end = offset + ph->curr_page_sum_size;
        uint32_t parsed_records = 0;
        while (parsed_records < ph->tb_count) {
            if (tu_format) {
                uint32_t magic = *(uint32_t *)(data + tb_off);
                if (magic == AOT_TU_REF_MAGIC) {
                    tb_off += sizeof(aot_tu_ref);
                    parsed_records++;
                    continue;
                }
                aot_tu_head *tu = (aot_tu_head *)(data + tb_off);
                aot_tu_page *pages = (aot_tu_page *)(tu + 1);
                aot_block *blocks = (aot_block *)(pages + tu->page_count);
                uint8_t *ir2_base = (uint8_t *)(blocks + tu->tb_count);
                uint8_t *rel_base = ir2_base + blocks[0].tu_size;

                for (uint32_t i = 0; i < tu->tb_count; i++) {
                    aot_tb_entry *e = append_aot_tb();
                    if (!e) {
                        g_hash_table_destroy(guest_pages);
                        return -1;
                    }
                    e->pc = blocks[i].pc;
                    e->cs_base = blocks[i].cs_base;
                    e->flags = blocks[i].flags;
                    e->cflags = blocks[i].cflags;
                    e->ir2_num = blocks[i].ir2_num;
                    e->ir2_len = blocks[i].ir2_num * 4;
                    e->ir2_data = ir2_base + blocks[i].tc.offset_in_tu;
                    int64_t page_delta = (int64_t)(
                        (blocks[i].pc & TARGET_PAGE_MASK) -
                        (tu->first_pc & TARGET_PAGE_MASK));
                    e->guest_code[0] = find_aot_tu_guest_page(guest_pages,
                            pages, tu->page_count, page_delta);
                    e->guest_code[1] = blocks[i].page_addr[1] ==
                            (tb_page_addr_t)-1 ? NULL :
                        find_aot_tu_guest_page(guest_pages, pages,
                                tu->page_count,
                                page_delta + TARGET_PAGE_SIZE);
                    if (blocks[i].rel_start >= 0) {
                        e->rel_count = blocks[i].rel_end -
                            blocks[i].rel_start + 1;
                        e->rel_data = rel_base + blocks[i].rel_start *
                            sizeof(aot_rel);
                    }
                    e->matched = 0;
                }
                parsed_records++;
                tb_off += tu->tu_sum_size;
                continue;
            }

            uint32_t magic = *(uint32_t *)(data + tb_off);
            uint32_t sum_size = *(uint32_t *)(data + tb_off + 4);
            uint32_t all_tb_size = *(uint32_t *)(data + tb_off + 8);
            uint32_t rel_count = *(uint32_t *)(data + tb_off + 12);
            if (magic != AOT_TB_MAGIC) break;
            aot_block *blk = (aot_block *)(data + tb_off + 16);
            uint32_t ir2_off = 16 + sizeof(aot_block);
            uint32_t ir2_len = blk->ir2_num * 4;
            aot_tb_entry *e = append_aot_tb();
            if (!e) {
                g_clear_pointer(&guest_pages, g_hash_table_destroy);
                return -1;
            }
            e->pc = blk->pc;
            e->cs_base = blk->cs_base;
            e->flags = blk->flags;
            e->cflags = blk->cflags;
            e->ir2_num = blk->ir2_num;
            e->ir2_len = ir2_len;
            e->ir2_data = ir2_len ? data + tb_off + ir2_off : NULL;
            e->guest_code[0] = guest_code;
            e->guest_code[1] = NULL;
            e->rel_count = rel_count;
            e->rel_data = rel_count ? data + tb_off + 16 + all_tb_size : NULL;
            e->matched = 0;
            tb_off += sum_size;
            parsed_records++;
        }
        if (tb_off != page_end) {
            g_clear_pointer(&guest_pages, g_hash_table_destroy);
            return -1;
        }
        offset = page_end;
    }
    g_clear_pointer(&guest_pages, g_hash_table_destroy);
    return 0;
}

static aot_tb_entry *find_aot_tb(uint64_t pc, uint32_t cs_base, uint32_t flags, uint32_t cflags) {
    for (int i = 0; i < g_aot_tb_count; i++) {
        aot_tb_entry *e = &g_aot_tbs[i];
        if (!e->matched && e->pc == pc && e->cs_base == cs_base &&
            e->flags == flags && e->cflags == cflags)
            return e;
    }
    return NULL;
}

static void generate_report(FILE *f) {
    int mismatch_ir1 = 0, mismatch_ir2 = 0, mismatch_meta = 0;
    for (int i = 0; i < g_mismatch_count; i++) {
        if (g_mismatches[i].mismatch_type & 1) mismatch_meta++;
        if (g_mismatches[i].mismatch_type & 2) mismatch_ir2++;
        if (g_mismatches[i].mismatch_type & 4) mismatch_ir1++;
    }
    fprintf(f, "\n============================================================\n");
    fprintf(f, "  AOT Verification Report\n");
    fprintf(f, "============================================================\n\n");
    fprintf(f, "Summary:\n");
    fprintf(f, "  TB Record:                  %d\n", g_rec_count);
    fprintf(f, "  AOT TB count:               %d\n", g_aot_tb_count);
    fprintf(f, "  AOT TB Hit:                 %d\n", g_total_matched);
    fprintf(f, "  AOT Hit but Mismatch:       %d  (details shown: %d, meta=%d  IR1=%d  IR2=%d)\n",
            g_total_mismatch_count, g_mismatch_count, mismatch_meta, mismatch_ir1, mismatch_ir2);
    fprintf(f, "  AOT TB Miss:             %d\n", g_unmatched_rec);
    fprintf(f, "  AOT TB not in record:       %d\n", g_unmatched_aot);
    int ok = g_total_matched - g_total_mismatch_count;
    fprintf(f, "  Verified OK:                %d\n", ok);
    if (g_total_mismatch_count == 0 && g_unmatched_rec == 0 && g_unmatched_aot == 0)
        fprintf(f, "\n  Result: PASS\n");
    else
        fprintf(f, "\n  Result: FAIL\n");

    for (int i = 0; i < g_mismatch_count; i++) {
        mismatch_info *m = &g_mismatches[i];
        fprintf(f, "\n  TB PC=0x%016" PRIx64 " (cs=0x%x flags=0x%x cflags=0x%x)\n",
                m->pc, m->cs_base, m->flags, m->cflags);
        if (m->mismatch_type & 1)
            fprintf(f, "    [METADATA] rec_ir2_len=%u aot_ir2_len=%u\n",
                    m->rec_ir2_len, m->aot_ir2_len);
        if (m->mismatch_type & 2) {
            fprintf(f, "    [IR2 MISMATCH]\n");
            ir2_side_by_side(f, m->rec_ir2, m->rec_ir2_len, m->aot_ir2, m->aot_ir2_len);
        }
        if (m->mismatch_type & 4) {
            fprintf(f, "    [IR1 MISMATCH] (%u bytes)\n", m->rec_ir1_len);
            fprintf(f, "      record:");
            for (uint32_t k = 0; k < m->rec_ir1_len; k++) {
                fprintf(f, " %02x", m->rec_ir1[k]);
            }
            fprintf(f, "\n      aot:   ");
            for (uint32_t k = 0; k < m->rec_ir1_len; k++) {
                fprintf(f, " %02x", aot_ir1_byte(m, k));
            }
            fprintf(f, "\n      diff:  ");
            for (uint32_t k = 0; k < m->rec_ir1_len; k++) {
                fprintf(f, " %s", m->rec_ir1[k] == aot_ir1_byte(m, k) ?
                        "  " : "^^");
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "\n============================================================\n");
}

int check_aot_verify_mode(int argc, char **argv) {
    latx_parse_verbose_early(argc, argv);

    for (int i = 1; i < argc; i++) {
        aot_verbose_log("check_aot_verify: checking argv[%d] = '%s'\n",
                i, argv[i]);
        fflush(stderr);
        if (strcmp(argv[i], "-aot-verify") == 0) {
            aot_verbose_log("check_aot_verify: FOUND at index %d\n", i);
            fflush(stderr);
            return i;
        }
    }
    aot_verbose_log("check_aot_verify: NOT FOUND - returning 0\n");
    fflush(stderr);
    return 0;
}

int run_aot_verify_mode(int argc, char **argv, int verify_idx) {
    const char *record_path = NULL;
    const char *aot_core_path = NULL;
    const char *aot_user_path = NULL;
    const char *report_path = NULL;

    optind = verify_idx + 1;
    int pos = 0;
    for (int i = optind; i < argc; i++) {
        if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        } else if (argv[i][0] != '-') {
            if (pos == 0) record_path = argv[i];
            else if (pos == 1) aot_core_path = argv[i];
            else if (pos == 2) aot_user_path = argv[i];
            pos++;
        }
    }
    if (!record_path || !aot_core_path) {
        fprintf(stderr, "Usage: %s -aot-verify <tb-record> <aot-core> [aot-user] [--report FILE]\n",
                argv[0]);
        return 1;
    }

    aot_verbose_log("AOT Verify Mode\n");

    /* Initialize aot_file_path to current directory so load_aot_file can find the files */
    aot_file_path = malloc(PATH_MAX);
    if (!aot_file_path) {
        fprintf(stderr, "Error: cannot allocate aot_file_path\n");
        return 1;
    }
    if (getcwd(aot_file_path, PATH_MAX) == NULL) {
        fprintf(stderr, "Error: cannot get current directory\n");
        free(aot_file_path);
        return 1;
    }
    /* Append trailing slash if needed */
    int len = strlen(aot_file_path);
    if (len > 0 && aot_file_path[len - 1] != '/') {
        strcat(aot_file_path, "/");
    }
    aot_verbose_log("AOT file path: %s\n", aot_file_path);

    tb_rec *recs = NULL;
    if (parse_tb_record_file(record_path, &recs, &g_rec_count) < 0) return 1;
    aot_verbose_log("Loaded %d TB records from %s\n", g_rec_count, record_path);

    option_sys_aot = 1;
    option_aot_save = 0;
    option_aot_load = 1;

    const char *core_name = aot_core_path;
    if (core_name[0] == '.' && core_name[1] == '/') {
        core_name += 2;
    }
    char core_path[PATH_MAX];
    snprintf(core_path, PATH_MAX, "%s%s", aot_file_path, core_name);
    uint64_t core_size = 0;
    void *core_data = load_aot_file_unsafe(core_path, &core_size);
    if (!core_data) {
        fprintf(stderr, "Error: cannot load AOT core file '%s'\n", core_path);
        free(recs); return 1;
    }
    if (parse_aot_data((uint8_t *)core_data, core_size) < 0) {
        fprintf(stderr, "Error: cannot parse AOT core file '%s'\n", core_path);
        munmap(core_data, core_size);
        free(recs);
        return 1;
    }
    aot_verbose_log("Parsed %d TBs from AOT core\n", g_aot_tb_count);

    void *user_data = NULL;
    uint64_t user_size = 0;
    if (aot_user_path) {
        const char *user_name = aot_user_path;
        if (user_name[0] == '.' && user_name[1] == '/') {
            user_name += 2;
        }
        char user_path[PATH_MAX];
        snprintf(user_path, PATH_MAX, "%s%s", aot_file_path, user_name);
        user_data = load_aot_file_unsafe(user_path, &user_size);
        if (user_data) {
            int before = g_aot_tb_count;
            if (parse_aot_data((uint8_t *)user_data, user_size) < 0) {
                fprintf(stderr, "Error: cannot parse AOT user file '%s'\n",
                        user_path);
                munmap(user_data, user_size);
                user_data = NULL;
            }
            aot_verbose_log("Parsed %d TBs from AOT user (total %d)\n",
                    g_aot_tb_count - before, g_aot_tb_count);
        }
    }

    for (int i = 0; i < g_rec_count; i++) {
        tb_rec *r = &recs[i];
        aot_tb_entry *aot = find_aot_tb(r->pc, r->cs_base, r->flags, r->cflags);
        if (!aot) {
            aot_verbose_log("TB[%d] Miss: pc 0x%016lx size 0x%x cs 0x%08x flags 0x%08x cflags 0x%08x\n",
                    i, r->pc, r->ir1_len, r->cs_base, r->flags, r->cflags);
            g_unmatched_rec++;
            continue;
	}
        aot->matched = 1;
        g_total_matched++;
        int mt = 0;

        uint32_t aot_ir2_len = aot->ir2_num * 4;
        if (aot_ir2_len != r->ir2_len) mt |= 1;

        if (aot->rel_count > 0) {
        } else {
            uint32_t cmp = r->ir2_len < aot_ir2_len ? r->ir2_len : aot_ir2_len;
            if (r->ir2_len != aot_ir2_len ||
                (r->ir2_data && aot->ir2_data && memcmp(r->ir2_data, aot->ir2_data, cmp) != 0))
                mt |= 2;
        }

        if (r->ir1_len > 0) {
            if (!aot_ir1_matches(aot, r)) {
                mt |= 4;
            }
        }
        if (mt) {
            g_total_mismatch_count++;
            if (g_mismatch_count < MAX_MISMATCHES) {
                mismatch_info *m = &g_mismatches[g_mismatch_count];
                m->pc = r->pc; m->cs_base = r->cs_base;
                m->flags = r->flags; m->cflags = r->cflags;
                m->rec_ir2_len = r->ir2_len; m->aot_ir2_len = aot_ir2_len;
                m->rec_ir2 = r->ir2_data; m->aot_ir2 = aot->ir2_data;
                m->rec_ir1_len = r->ir1_len;
                m->rec_ir1 = r->ir1_data;
                m->aot_guest_code[0] = aot->guest_code[0];
                m->aot_guest_code[1] = aot->guest_code[1];
                m->mismatch_type = mt;
                g_mismatch_count++;
            }
        }
    }

    for (int i = 0; i < g_aot_tb_count; i++)
        if (!g_aot_tbs[i].matched) g_unmatched_aot++;

    FILE *rf = report_path ? fopen(report_path, "w") : stdout;
    if (rf) { generate_report(rf); if (report_path) fclose(rf); }

    for (int i = 0; i < g_rec_count; i++) {
        free(recs[i].ir1_data);
        free(recs[i].ir2_data);
    }
    free(recs);
    munmap(core_data, core_size);
    if (user_data) {
        munmap(user_data, user_size);
    }
    free(g_aot_tbs);
    g_aot_tbs = NULL;
    g_aot_tb_count = 0;
    g_aot_tb_capacity = 0;
    free(aot_file_path);
    aot_file_path = NULL;

    aot_verbose_log("Verification complete: %d matched, %d mismatched\n",
            g_total_matched, g_mismatch_count);
    return (g_mismatch_count > 0 || g_unmatched_rec > 0) ? 2 : 0;
}
