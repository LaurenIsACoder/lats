/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Restore guest state from compiler-provided native fault locations. */
#include "latx-native-recover.h"
#include "lats/aot-atomic.h"
#include "ir1.h"

/* Called only after restoring the exact guest instruction's pre-state.
 * A host load can be the read half of an x86 memory read/modify/write. */
static bool hamt_native_guest_rmw(CPUX86State *env)
{
    uint8_t code[15];
    uint64_t pc = env->eip;
    size_t bytes = MIN(sizeof(code), TARGET_PAGE_SIZE - (pc & ~TARGET_PAGE_MASK));
    struct {
        cs_insn insn;
        cs_detail detail;
    } decoded;
    cs_insn *insn = NULL;
    size_t count;
    bool write = false;

    if (!(env->hflags & HF_CS64_MASK)) {
        return false;
    }
    if (cpu_memory_rw_debug(env_cpu(env), pc, code, bytes, false)) {
        error_report("native recovery guest instruction read failed");
        abort();
    }
    QEMU_BUILD_BUG_ON(offsetof(typeof(decoded), detail) != sizeof(cs_insn));
    count = cs_disasm(handle64, code, bytes, pc, 1, &insn, 0, &decoded);
    if (!count && bytes < sizeof(code)) {
        /* A short instruction at a page end need not read the next page. */
        if (cpu_memory_rw_debug(env_cpu(env), pc + bytes, code + bytes,
                                sizeof(code) - bytes, false)) {
            error_report("native recovery cross-page instruction read failed");
            abort();
        }
        count = cs_disasm(handle64, code, sizeof(code), pc, 1, &insn, 0,
                          &decoded);
    }
    if (count != 1 || !insn->detail) {
        error_report("native recovery guest instruction decode failed");
        abort();
    }
    /* Do not rely on access metadata omitted by some decoder builds.
     * PUSH/CALL source reads, CMP and TEST must remain read accesses. */
    switch (insn->id) {
    case X86_INS_ADD: case X86_INS_ADC: case X86_INS_SUB: case X86_INS_SBB:
    case X86_INS_AND: case X86_INS_OR: case X86_INS_XOR:
    case X86_INS_INC: case X86_INS_DEC:
    case X86_INS_NEG: case X86_INS_NOT:
    case X86_INS_SHL: case X86_INS_SAL: case X86_INS_SHR: case X86_INS_SAR:
    case X86_INS_ROL: case X86_INS_ROR: case X86_INS_RCL: case X86_INS_RCR:
    case X86_INS_SHLD: case X86_INS_SHRD:
    case X86_INS_BTC: case X86_INS_BTR: case X86_INS_BTS:
    case X86_INS_XADD:
    case X86_INS_CMPXCHG: case X86_INS_CMPXCHG8B: case X86_INS_CMPXCHG16B:
        write = insn->detail->x86.op_count &&
                insn->detail->x86.operands[0].type == X86_OP_MEM;
        break;
    case X86_INS_XCHG:
        write = (insn->detail->x86.op_count &&
                 insn->detail->x86.operands[0].type == X86_OP_MEM) ||
                (insn->detail->x86.op_count > 1 &&
                 insn->detail->x86.operands[1].type == X86_OP_MEM);
        break;
    default:
        break;
    }
    return write;
}


static const AOTNativeRecord *hamt_native_fault_record(uintptr_t epc)
{
    if (!aot_tu_enabled() || !option_aot_load) {
        return NULL;
    }
    TranslationBlock *tb = tcg_tb_lookup(epc);
    if (!tb || !TB_TU_PAGES(tb) || !TB_TU_PAGES(tb)->native) {
        return NULL;
    }
    AOTNativeTUInfo *info = TB_TU_PAGES(tb)->native;
    for (unsigned i = 0; i < info->count; ++i) {
        const AOTNativeRecord *record = &info->records[i];
        uintptr_t begin = (uintptr_t)record->entry;
        if (record->recover && epc >= begin && epc - begin < record->entry_bytes) {
            return record;
        }
    }
    return NULL;
}

/* A failed atomic has not committed guest state. Let its native failure
 * branch leave LLVM through the existing terminal atomic entry. */
static bool hamt_native_atomic_fallback(CPUX86State *env, uintptr_t epc)
{
    const AOTNativeRecord *record = hamt_native_fault_record(epc);
    const NativeRecoverHeader *header = record ? record->recover : NULL;
    if (!record || epc - (uintptr_t)record->entry > header->code_bytes ||
        header->code_bytes - (epc - (uintptr_t)record->entry) <
            LATS_HAMT_ATOMIC_WORDS * sizeof(uint32_t) ||
        !lats_aot_hamt_atomic_sequence((const uint32_t *)epc)) {
        return false;
    }
    unsigned cpuid = qemu_tcg_mttcg_enabled() ? env_cpu(env)->cpu_index : 0;
    ((uint64_t *)hamt_data_area[cpuid])[PT_ERA_INDEX] += 8;
    return true;
}

/* C receives a normal userspace stack alias; State still uses DMW pointers. */
static bool hamt_native_recover(CPUX86State *env, uintptr_t epc, uint64_t badv)
{
    const AOTNativeRecord *record = hamt_native_fault_record(epc);
    if (!record) {
        return false;
    }
    const NativeRecoverHeader *h = record->recover;
    if (!h->state_bytes && !h->points) {
        /* Direct LLVM code relies on the ordinary HAMT access handler. */
        return false;
    }
    bool direct = h->state_bytes == 0;
    const uint8_t *pos = (const void *)(h + 1);
    const NativeRecoverPoint *point = NULL;
    for (unsigned i = 0; i < h->points; ++i) {
        const NativeRecoverPoint *candidate = (const void *)pos;
        if (candidate->native_offset == epc - (uintptr_t)record->entry) {
            point = candidate;
            break;
        }
        pos += sizeof(*candidate) + candidate->locations * sizeof(NativeRecoverLocation);
    }
    if (!point) {
        /* Never use an approximate function-entry state for an unmapped PC. */
        error_report("unmapped instruction inside native recovery function: "
                     "offset=%" PRIxPTR " instruction=%08" PRIx32
                     " badv=%" PRIx64,
                     epc - (uintptr_t)record->entry, *(uint32_t *)epc, badv);
        abort();
    }
    if (*(uint32_t *)epc != point->instruction) {
        error_report("native recovery instruction mismatch");
        abort();
    }
    CPUState *cpu = env_cpu(env);
    int cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;
    uint64_t *frame = (void *)hamt_data_area[cpuid];
    const NativeRecoverLocation *locations = (const void *)(point + 1);
    uint64_t values[43] = {0};
    uint8_t vectors[16][16];
    uint64_t fields = 0;
    uint8_t scratch[16];
    /* State itself may be spilled. Read its slot using the captured fault SP,
     * validate the resulting outer frame, and only then dereference State. */
    uint8_t *state = NULL;
    uint64_t state_address = 0;
    if (!direct) {
        bool have_state = false;
        for (unsigned i = 0; i < point->locations; ++i) {
            const NativeRecoverLocation *loc = &locations[i];
            if (loc->field == 36) {
                if (!native_recover_read_location(loc, frame, scratch, &values[36])) {
                    error_report("invalid native recovery State location");
                    abort();
                }
                have_state = true;
                break;
            }
        }
        uintptr_t state_normal;
        state_address = values[36];
        if (!have_state || !native_recover_private_range(state_address,
                                                        h->frame_bytes, &state_normal)) {
            error_report("invalid native recovery State frame");
            abort();
        }
        state = (void *)state_normal;
    }
    for (unsigned i = 0; i < point->locations; ++i) {
        const NativeRecoverLocation *loc = &locations[i];
        if (loc->field != 36) {
            if (!native_recover_read_location(loc, frame, scratch, &values[loc->field])) {
                error_report("invalid native recovery location");
                abort();
            }
        }
        if (loc->field >= 16 && loc->field < 32) {
            memcpy(vectors[loc->field - 16], scratch, 16);
        }
        fields |= UINT64_C(1) << loc->field;
    }
    if (badv - values[37] >= point->memory_bytes) {
        error_report("native recovery effective address mismatch");
        abort();
    }
    uint64_t mask = (fields & (UINT64_C(1) << 39)) ? values[39] : point->flags_mask;
    uint64_t flags;
    memcpy(&flags, record->core && !direct ? state + 128 :
           (uint8_t *)frame + NATIVE_FLAGS_OFFSET, 8);
    if (!native_recover_flags(flags, mask, fields, values, &flags)) {
        error_report("invalid native recovery flags mask or fields");
        abort();
    }
    latxs_ras_tp_save(env, frame);
    for (unsigned i = 0; i < 16; ++i) {
        if (direct) {
            env->regs[i] = values[i];
        } else {
            memcpy(&env->regs[i], state + i * 8, 8);
            if (fields & (UINT64_C(1) << i)) {
                env->regs[i] = values[i];
            }
        }
    }
    for (unsigned i = 0; i < 16; ++i) {
        unsigned physical = 16 + i;
        const uint8_t *original = record->core && h->state_bytes == 672 ?
            state + 160 + i * 32 :
            (h->saved_xr_mask & (1u << physical)) ?
            state + h->saved_xr_offsets[physical] :
            (uint8_t *)frame + NATIVE_XR_OFFSET + physical * 32;
        memcpy(&env->xmm_regs[i]._b_ZMMReg[0], original, 16);
        if (fields & (UINT64_C(1) << (16 + i))) {
            memcpy(&env->xmm_regs[i]._b_ZMMReg[0], vectors[i], 16);
        }
    }
#ifdef CONFIG_LATX_OPT_NOFP
    bool sync_fprs = env->nofp != 0;
#else
    bool sync_fprs = !option_soft_fpu_fast || !env->mode_fpu;
#endif
    if (sync_fprs) {
        for (unsigned i = 0; i < 8; ++i) {
            const uint8_t *original = (h->saved_xr_mask & (1u << i)) ?
                state + h->saved_xr_offsets[i] :
                (uint8_t *)frame + NATIVE_XR_OFFSET + i * 32;
            memcpy(&env->fpregs[i].d.low, original, 8);
        }
#ifdef CONFIG_LATX_OPT_NOFP
        env->nofp = 0;
#endif
    }
    if (!option_soft_fpu) {
        env->fpstt = lsfpu_get_ftop();
        leave_lsfpu_mode();
    }
    env->cc_op = CC_OP_EFLAGS;
    env->cc_src = flags & (CC_C | CC_P | CC_A | CC_Z | CC_S | CC_O);
    env->cc_dst = 0;
#ifdef CONFIG_LATX_OPT_EFLAGS
    /* cpu_cc_compute_all reads live LBT flags in this configuration. */
    __lbt_mtflag_all(env->cc_src);
#endif
    uint32_t fcsr;
    memcpy(&fcsr, (uint8_t *)frame + NATIVE_FCSR_OFFSET, sizeof(fcsr));
    __write_native_fcsr0(fcsr);
    uint64_t guest_anchor = record->tb->pc;
    if (record->core && !direct) {
        memcpy(&guest_anchor, state + 136, sizeof(guest_anchor));
    }
    env->eip = guest_anchor + point->guest_offset;
    env->native_recovered = true;
    return true;
}
