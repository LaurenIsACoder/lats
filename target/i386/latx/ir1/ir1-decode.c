/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "../include/ir1-decode.h"

#define IR1_DECODE_TEMPLATE_CACHE_SIZE 16384

/*
 * Translation decodes repeated position-independent instruction encodings.
 * Keep exact decoded instructions as templates after the hot decoder declines
 * them, and retain the full decoder for unsupported forms and cache misses.
 */
typedef struct Ir1DecodeTemplate {
    uint8_t mode;
    uint8_t size;
    uint8_t bytes[15];
    cs_insn decoded;
    cs_detail detail;
} Ir1DecodeTemplate;

static __thread Ir1DecodeTemplate *ir1_decode_templates;
static __thread uint8_t *ir1_decode_template_valid;
static GPrivate ir1_decode_templates_owner = G_PRIVATE_INIT(g_free);
static GPrivate ir1_decode_template_valid_owner = G_PRIVATE_INIT(g_free);

static uint32_t ir1_decode_template_hash(const uint8_t *code, size_t size,
                                         int mode)
{
    uint32_t hash = 2166136261u ^ (uint32_t)mode;
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ code[i]) * 16777619u;
    }
    return hash & (IR1_DECODE_TEMPLATE_CACHE_SIZE - 1);
}

static cs_insn *ir1_decode_template_output(int ir1_num,
                                                     void *pir1_base)
{
    return (void *)((uintptr_t)pir1_base +
                    ir1_num * IR1_INST_SIZE);
}

static void ir1_decode_output_init(cs_insn *info)
{
    memset(info, 0, IR1_INST_SIZE);
    info->detail = (void *)(info + 1);
}

/*
 * Decode the common, regular x86-64 forms directly.  This path runs before the
 * cache, has no lookup or warm-up cost, and rejects prefixes and forms that it
 * does not model exactly.  Rejected instructions continue through the exact
 * cache and then the full decoder.
 */
static const x86_reg ir1_hot_gpr64[16] = {
    X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
    X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
    X86_REG_R8, X86_REG_R9, X86_REG_R10, X86_REG_R11,
    X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15,
};

static const x86_reg ir1_hot_gpr32[16] = {
    X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_EBX,
    X86_REG_ESP, X86_REG_EBP, X86_REG_ESI, X86_REG_EDI,
    X86_REG_R8D, X86_REG_R9D, X86_REG_R10D, X86_REG_R11D,
    X86_REG_R12D, X86_REG_R13D, X86_REG_R14D, X86_REG_R15D,
};

static int64_t ir1_hot_read_simm(const uint8_t *code, uint8_t size)
{
    if (size == 1) {
        return *(const int8_t *)code;
    }
    int32_t value;
    memcpy(&value, code, sizeof(value));
    return value;
}

static void ir1_hot_finish(cs_insn *info, const uint8_t *code,
                           uint8_t size, uint64_t address, uint8_t rex,
                           uint8_t opcode0, uint8_t opcode1)
{
    info->size = size;
    info->address = address;
    info->detail->x86.addr_size = 8;
    info->detail->x86.rex = rex;
    info->detail->x86.opcode[0] = opcode0;
    info->detail->x86.opcode[1] = opcode1;
    memcpy(info->bytes, code, size);
    cs_x86 *x86 = &info->detail->x86;
    uint8_t offset = rex ? 2 : 1;
    if (opcode0 >= 0xb8 && opcode0 <= 0xbf) {
        x86->encoding.imm_offset = offset;
        x86->encoding.imm_size = size - offset;
        if (rex & 8) {
            info->id = X86_INS_MOVABS;
        }
    } else if ((opcode0 >= 0x70 && opcode0 <= 0x7f) ||
               opcode0 == 0xe8 || opcode0 == 0xe9 || opcode0 == 0xeb ||
               (opcode0 == 0x0f && opcode1 >= 0x80 && opcode1 <= 0x8f)) {
        x86->encoding.imm_offset = opcode0 == 0x0f ? 2 : 1;
        x86->encoding.imm_size = size - x86->encoding.imm_offset;
    } else if (x86->op_count == 2) {
        x86->encoding.modrm_offset = offset;
        uint8_t modrm = code[offset++];
        uint8_t mod = modrm >> 6;
        uint8_t rm = modrm & 7;
        bool disp32 = mod == 0 && rm == 5;
        if (mod != 3 && rm == 4) {
            uint8_t sib = code[offset++];
            uint8_t index = (sib >> 3) & 7;
            uint8_t base = sib & 7;
            x86->sib = sib;
            x86->sib_scale = 1 << (sib >> 6);
            if (index != 4 || (rex & 2)) {
                x86->sib_index = ir1_hot_gpr64[index | ((rex & 2) << 2)];
            }
            if (mod == 0 && base == 5) {
                disp32 = true;
            } else {
                x86->sib_base = ir1_hot_gpr64[base | ((rex & 1) << 3)];
            }
        }
        if (mod == 1 || mod == 2 || disp32) {
            x86->encoding.disp_offset = offset;
            x86->encoding.disp_size = mod == 1 ? 1 : 4;
            x86->disp = ir1_hot_read_simm(code + offset,
                                         x86->encoding.disp_size);
        }
        if (opcode0 == 0x83) {
            x86->encoding.imm_offset = size - 1;
            x86->encoding.imm_size = 1;
        }
    }
#ifndef CONFIG_CAPSTONE_DIET
    for (int i = 0; i < info->detail->x86.op_count; i++) {
        cs_x86_op *operand = &info->detail->x86.operands[i];
        if (operand->type != X86_OP_IMM) {
            operand->access = CS_AC_READ;
        }
    }
    switch (info->id) {
    case X86_INS_MOV:
    case X86_INS_LEA:
    case X86_INS_POP:
        info->detail->x86.operands[0].access = CS_AC_WRITE;
        break;
    case X86_INS_ADD:
    case X86_INS_OR:
    case X86_INS_ADC:
    case X86_INS_SBB:
    case X86_INS_AND:
    case X86_INS_SUB:
    case X86_INS_XOR:
        info->detail->x86.operands[0].access = CS_AC_READ | CS_AC_WRITE;
        break;
    default:
        break;
    }
#endif
}

static uint8_t ir1_hot_decode_rm(const uint8_t *code, uint8_t offset,
                                 uint8_t rex, uint8_t width,
                                 cs_x86_op *operand)
{
    uint8_t modrm = code[offset++];
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;
    if (mod == 3) {
        const x86_reg *regs = width == 8 ? ir1_hot_gpr64 : ir1_hot_gpr32;
        operand->type = X86_OP_REG;
        operand->size = width;
        operand->reg = regs[rm | ((rex & 1) << 3)];
        return offset;
    }

    operand->type = X86_OP_MEM;
    operand->size = width;
    operand->mem.segment = X86_REG_INVALID;
    operand->mem.base = X86_REG_INVALID;
    operand->mem.index = X86_REG_INVALID;
    operand->mem.scale = 1;
    operand->mem.disp = 0;

    bool disp32 = false;
    if (rm == 4) {
        uint8_t sib = code[offset++];
        uint8_t index = (sib >> 3) & 7;
        uint8_t base = sib & 7;
        operand->mem.scale = 1 << (sib >> 6);
        if (index != 4 || (rex & 2)) {
            operand->mem.index = ir1_hot_gpr64[index | ((rex & 2) << 2)];
        }
        if (mod == 0 && base == 5) {
            disp32 = true;
        } else {
            operand->mem.base = ir1_hot_gpr64[base | ((rex & 1) << 3)];
        }
    } else if (mod == 0 && rm == 5) {
        operand->mem.base = X86_REG_RIP;
        disp32 = true;
    } else {
        operand->mem.base = ir1_hot_gpr64[rm | ((rex & 1) << 3)];
    }
    if (mod == 1) {
        operand->mem.disp = ir1_hot_read_simm(code + offset, 1);
        offset++;
    } else if (mod == 2 || disp32) {
        operand->mem.disp = ir1_hot_read_simm(code + offset, 4);
        offset += 4;
    }
    return offset;
}

static cs_insn *ir1_hot_decode(const uint8_t *code,
                                         uint64_t address, int ir1_num,
                                         void *pir1_base, int mode)
{
    static const unsigned int jcc[16] = {
        X86_INS_JO, X86_INS_JNO, X86_INS_JB, X86_INS_JAE,
        X86_INS_JE, X86_INS_JNE, X86_INS_JBE, X86_INS_JA,
        X86_INS_JS, X86_INS_JNS, X86_INS_JP, X86_INS_JNP,
        X86_INS_JL, X86_INS_JGE, X86_INS_JLE, X86_INS_JG,
    };
    /* Non-diet/debug builds retain textual and implicit-register metadata. */
#if !defined(CONFIG_CAPSTONE_DIET) || defined(CONFIG_LATX_DEBUG)
    return NULL;
#endif
    if (mode != 64 || !pir1_base) {
        return NULL;
    }
    uint8_t offset = 0;
    uint8_t rex = 0;
    if (code[offset] >= 0x40 && code[offset] <= 0x4f) {
        rex = code[offset++];
    }
    uint8_t opcode = code[offset++];
    cs_insn *info;

    if (!rex && opcode >= 0x70 && opcode <= 0x7f) {
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = jcc[opcode & 0xf];
        info->detail->x86.op_count = 1;
        info->detail->x86.operands[0].type = X86_OP_IMM;
        info->detail->x86.operands[0].size = 8;
        info->detail->x86.operands[0].imm = address + 2 +
                                     ir1_hot_read_simm(code + 1, 1);
        ir1_hot_finish(info, code, 2, address, 0, opcode, 0);
        return info;
    }
    if (!rex && opcode == 0x0f && code[1] >= 0x80 && code[1] <= 0x8f) {
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = jcc[code[1] & 0xf];
        info->detail->x86.op_count = 1;
        info->detail->x86.operands[0].type = X86_OP_IMM;
        info->detail->x86.operands[0].size = 8;
        info->detail->x86.operands[0].imm = address + 6 +
                                     ir1_hot_read_simm(code + 2, 4);
        ir1_hot_finish(info, code, 6, address, 0, 0x0f, code[1]);
        return info;
    }
    if (!rex && (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb)) {
        uint8_t imm_size = opcode == 0xeb ? 1 : 4;
        uint8_t size = 1 + imm_size;
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = opcode == 0xe8 ? X86_INS_CALL : X86_INS_JMP;
        info->detail->x86.op_count = 1;
        info->detail->x86.operands[0].type = X86_OP_IMM;
        info->detail->x86.operands[0].size = 8;
        info->detail->x86.operands[0].imm = address + size +
                                     ir1_hot_read_simm(code + 1, imm_size);
        ir1_hot_finish(info, code, size, address, 0, opcode, 0);
        return info;
    }
    if (!rex && (opcode == 0x90 || opcode == 0xc3)) {
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = opcode == 0x90 ? X86_INS_NOP : X86_INS_RET;
        ir1_hot_finish(info, code, 1, address, 0, opcode, 0);
        return info;
    }
    if (opcode >= 0x50 && opcode <= 0x5f && !(rex & 0x0e)) {
        unsigned int reg = (opcode & 7) | ((rex & 1) << 3);
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = opcode < 0x58 ? X86_INS_PUSH : X86_INS_POP;
        info->detail->x86.op_count = 1;
        info->detail->x86.operands[0].type = X86_OP_REG;
        info->detail->x86.operands[0].size = 8;
        info->detail->x86.operands[0].reg = ir1_hot_gpr64[reg];
        ir1_hot_finish(info, code, offset, address, rex, opcode, 0);
        return info;
    }
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        uint8_t width = rex & 8 ? 8 : 4;
        uint64_t imm = 0;
        memcpy(&imm, code + offset, width);
        unsigned int reg = (opcode & 7) | ((rex & 1) << 3);
        const x86_reg *regs = width == 8 ? ir1_hot_gpr64 : ir1_hot_gpr32;
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = X86_INS_MOV;
        info->detail->x86.op_count = 2;
        info->detail->x86.operands[0].type = X86_OP_REG;
        info->detail->x86.operands[0].size = width;
        info->detail->x86.operands[0].reg = regs[reg];
        info->detail->x86.operands[1].type = X86_OP_IMM;
        info->detail->x86.operands[1].size = width;
        info->detail->x86.operands[1].imm = imm;
        ir1_hot_finish(info, code, offset + width, address, rex, opcode, 0);
        return info;
    }

    if (opcode == 0x83) {
        uint8_t modrm = code[offset];
        uint8_t group = (modrm >> 3) & 7;
        static const unsigned int group_ids[8] = {
            X86_INS_ADD, X86_INS_OR, X86_INS_ADC, X86_INS_SBB,
            X86_INS_AND, X86_INS_SUB, X86_INS_XOR, X86_INS_CMP,
        };
        uint8_t width = rex & 8 ? 8 : 4;
        cs_x86_op rm_operand = { 0 };
        uint8_t end = ir1_hot_decode_rm(code, offset, rex, width, &rm_operand);
        info = ir1_decode_template_output(ir1_num, pir1_base);
        ir1_decode_output_init(info);
        info->id = group_ids[group];
        info->detail->x86.modrm = modrm;
        info->detail->x86.op_count = 2;
        info->detail->x86.operands[0] = rm_operand;
        info->detail->x86.operands[1].type = X86_OP_IMM;
        info->detail->x86.operands[1].size = width;
        int64_t imm = ir1_hot_read_simm(code + end, 1);
        bool logical = group == 1 || group == 4 || group == 6;
        info->detail->x86.operands[1].imm = width == 4 && logical ?
                                     (uint32_t)imm : imm;
        ir1_hot_finish(info, code, end + 1, address, rex, opcode, 0);
        return info;
    }

    uint8_t modrm = code[offset];
    bool reverse = false;
    bool lea = false;
    unsigned int id;
    switch (opcode) {
    case 0x89:
        id = X86_INS_MOV;
        break;
    case 0x8b:
        id = X86_INS_MOV;
        reverse = true;
        break;
    case 0x8d:
        id = X86_INS_LEA;
        reverse = true;
        lea = true;
        break;
    case 0x85:
        id = X86_INS_TEST;
        break;
    case 0x31:
        id = X86_INS_XOR;
        break;
    case 0x33:
        id = X86_INS_XOR;
        reverse = true;
        break;
    case 0x39:
        id = X86_INS_CMP;
        break;
    case 0x3b:
        id = X86_INS_CMP;
        reverse = true;
        break;
    case 0x01:
        id = X86_INS_ADD;
        break;
    case 0x03:
        id = X86_INS_ADD;
        reverse = true;
        break;
    case 0x29:
        id = X86_INS_SUB;
        break;
    case 0x2b:
        id = X86_INS_SUB;
        reverse = true;
        break;
    case 0x21:
        id = X86_INS_AND;
        break;
    case 0x23:
        id = X86_INS_AND;
        reverse = true;
        break;
    case 0x09:
        id = X86_INS_OR;
        break;
    case 0x0b:
        id = X86_INS_OR;
        reverse = true;
        break;
    default: return NULL;
    }
    unsigned int reg_index = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    uint8_t width = rex & 8 ? 8 : 4;
    const x86_reg *regs = width == 8 ? ir1_hot_gpr64 : ir1_hot_gpr32;
    cs_x86_op rm_operand = { 0 };
    uint8_t end = ir1_hot_decode_rm(code, offset, rex, width, &rm_operand);
    if (lea && rm_operand.type != X86_OP_MEM) {
        return NULL;
    }
    /* Leave memory TEST's backend-specific access metadata to Capstone. */
    if (id == X86_INS_TEST && rm_operand.type == X86_OP_MEM) {
        return NULL;
    }
    cs_x86_op reg_operand = { 0 };
    reg_operand.type = X86_OP_REG;
    reg_operand.size = width;
    reg_operand.reg = regs[reg_index];
    info = ir1_decode_template_output(ir1_num, pir1_base);
    ir1_decode_output_init(info);
    info->id = id;
    info->detail->x86.modrm = modrm;
    info->detail->x86.op_count = 2;
    info->detail->x86.operands[0] = reverse ? reg_operand : rm_operand;
    info->detail->x86.operands[1] = reverse ? rm_operand : reg_operand;
    ir1_hot_finish(info, code, end, address, rex, opcode, 0);
    return info;
}

static cs_insn *ir1_decode_template_lookup(const uint8_t *code,
                                                     size_t code_size,
                                                     uint64_t address,
                                                     int ir1_num,
                                                     void *pir1_base,
                                                     int mode)
{
    if (!ir1_decode_templates || !pir1_base) {
        return NULL;
    }
    size_t prefixes = MIN(code_size, 4);
    for (size_t size = 1; size <= prefixes; size++) {
        size_t index = ir1_decode_template_hash(code, size, mode);
        Ir1DecodeTemplate *entry = &ir1_decode_templates[index];
        if (ir1_decode_template_valid[index] && entry->mode == mode &&
            entry->size <= code_size &&
            !memcmp(entry->bytes, code, entry->size)) {
            cs_insn *output = ir1_decode_template_output(
                ir1_num, pir1_base);
            memcpy(output, &entry->decoded, sizeof(*output));
            output->detail = (void *)(output + 1);
            memcpy(output->detail, &entry->detail, sizeof(entry->detail));
            output->address = address;
            return output;
        }
    }
    return NULL;
}

static bool ir1_decode_template_is_address_independent(
        const cs_insn *info)
{
    /* Relative immediates and RIP-relative memory change with the guest PC. */
    for (int i = 0; i < info->detail->x86.op_count; i++) {
        if (info->detail->x86.operands[i].type == X86_OP_IMM ||
            (info->detail->x86.operands[i].type == X86_OP_MEM &&
             (info->detail->x86.operands[i].mem.base == X86_REG_RIP ||
              info->detail->x86.operands[i].mem.index == X86_REG_RIP))) {
            return false;
        }
    }
    return true;
}

static void ir1_decode_template_insert(const uint8_t *code, size_t code_size,
                                       int mode,
                                       const cs_insn *info)
{
    if (!info->size || info->size > code_size || info->size > 15 ||
        !ir1_decode_template_is_address_independent(info)) {
        return;
    }
    if (!ir1_decode_templates) {
        ir1_decode_templates = g_new(Ir1DecodeTemplate,
                                     IR1_DECODE_TEMPLATE_CACHE_SIZE);
        ir1_decode_template_valid = g_new0(uint8_t,
                                           IR1_DECODE_TEMPLATE_CACHE_SIZE);
        g_private_set(&ir1_decode_templates_owner, ir1_decode_templates);
        g_private_set(&ir1_decode_template_valid_owner,
                      ir1_decode_template_valid);
    }
    size_t prefix_size = MIN((size_t)info->size, 4);
    size_t index = ir1_decode_template_hash(code, prefix_size, mode);
    Ir1DecodeTemplate *entry = &ir1_decode_templates[index];
    entry->mode = mode;
    entry->size = info->size;
    memcpy(entry->bytes, code, info->size);
    memcpy(&entry->decoded, info, sizeof(entry->decoded));
    entry->decoded.detail = NULL;
    memcpy(&entry->detail, info->detail, sizeof(entry->detail));
    ir1_decode_template_valid[index] = true;
}

/* Cache the raw decode, before per-translation segment/operand fixups. */
size_t latxs_decode_one(csh decoder, const uint8_t *code, uint64_t address,
                        int mode, cs_insn **insn, int ir1_num, void *base)
{
    cs_insn *info = ir1_hot_decode(code, address, ir1_num, base, mode);
    if (!info) {
        info = ir1_decode_template_lookup(code, 15, address,
                                          ir1_num, base, mode);
    }
    if (info) {
        *insn = info;
        return 1;
    }
    size_t count = cs_disasm(decoder, code, 15, address, 1,
                            insn, ir1_num, base);
    if (count == 1 && *insn && (*insn)->detail) {
        ir1_decode_template_insert(code, 15, mode, *insn);
    }
    return count;
}
