/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "qemu/error-report.h"
#include "lats/aot-atomic.h"
#include "tcg/helper-tcg.h"

void latx_helper_aot_atomic_request(CPUX86State *env, unsigned descriptor)
{
    g_assert(!env->aot_native_atomic.descriptor &&
             LATS_ATOMIC_LENGTH(descriptor));
    env->aot_native_atomic.descriptor = descriptor;
    /*
     * The static entry committed GPR, SIMD, flags and x87 state before C.
     * Do not replace that state with C's physical registers on longjmp.
     */
    env->native_recovered = true;
    env->is_fcsr_simd = 0;
    /*
     * Unlike a TCG atomic retry, this request also works when the normal
     * single-vCPU code was not marked CF_PARALLEL. It never retries a TB.
     */
    CPUState *cpu = env_cpu(env);
    g_assert(!cpu_in_exclusive_context(cpu));
    cpu->exception_index = EXCP_ATOMIC;
    cpu_loop_exit(cpu);
}

static uint64_t arithmetic_flags(uint64_t a, uint64_t b, uint64_t result,
                                 unsigned bits, bool subtract)
{
    uint64_t sign = UINT64_C(1) << (bits - 1);
    uint64_t overflow = (a ^ result) & (subtract ? a ^ b : ~(a ^ b));
    return (!__builtin_parity((unsigned char)result) ? CC_P : 0) |
           (((a ^ b ^ result) & 16) ? CC_A : 0) | (!result ? CC_Z : 0) |
           ((result & sign) ? CC_S : 0) | ((overflow & sign) ? CC_O : 0);
}

static uint64_t partial_register(uint64_t previous, uint64_t value,
                                 unsigned bits, unsigned high_byte)
{
    if (bits >= 32) {
        return value;
    }
    unsigned shift = high_byte ? 8 : 0;
    uint64_t mask = ((UINT64_C(1) << bits) - 1) << shift;
    return (previous & ~mask) | (value << shift);
}

static uint64_t atomic_value(CPUX86State *env, uint32_t descriptor,
                             uint64_t previous, uint64_t before,
                             uint64_t *result_flags)
{
    LatxNativeAtomicRequest *request = &env->aot_native_atomic;
    unsigned bits = LATS_ATOMIC_BYTES(descriptor) * 8;
    unsigned operation = LATS_ATOMIC_OPERATION(descriptor);
    uint64_t mask = UINT64_MAX >> (64 - bits);
    uint64_t source = request->value & mask,
             accumulator = env->regs[R_EAX] & mask;
    bool pair = LATS_ATOMIC_PAIR(descriptor);
    if (pair) {
        accumulator = (uint32_t)env->regs[R_EAX] |
                      ((uint64_t)(uint32_t)env->regs[R_EDX] << 32);
    }
    uint64_t a = previous, b = source, result = previous;
    uint64_t flags = before;
    bool subtract = false, change_flags = true, carry = false;
    __uint128_t wide;
    switch (operation) {
    case LATS_ATOMIC_ADD:
    case LATS_ATOMIC_XADD:
    case LATS_ATOMIC_INC:
    case LATS_ATOMIC_ADC:
        if (operation == LATS_ATOMIC_INC) {
            b = 1;
        }
        wide = (__uint128_t)a + b +
               (operation == LATS_ATOMIC_ADC && (before & CC_C) ? 1 : 0);
        result = (uint64_t)wide & mask;
        carry = (wide >> bits) != 0;
        break;
    case LATS_ATOMIC_SUB:
    case LATS_ATOMIC_DEC:
    case LATS_ATOMIC_SBB:
    case LATS_ATOMIC_NEG:
    case LATS_ATOMIC_CMPXCHG:
        if (operation == LATS_ATOMIC_DEC) {
            b = 1;
        }
        if (operation == LATS_ATOMIC_NEG) {
            a = 0;
            b = previous;
        }
        if (operation == LATS_ATOMIC_CMPXCHG && pair) {
            flags = (before & ~CC_Z) | (accumulator == previous ? CC_Z : 0);
            result = accumulator == previous ? source : previous;
            change_flags = false;
            break;
        }
        if (operation == LATS_ATOMIC_CMPXCHG) {
            a = accumulator;
            b = previous;
        }
        wide = (__uint128_t)b +
               (operation == LATS_ATOMIC_SBB && (before & CC_C) ? 1 : 0);
        result = (a - (uint64_t)wide) & mask;
        carry = (__uint128_t)a < wide;
        subtract = true;
        break;
    case LATS_ATOMIC_BIT: {
        unsigned kind = LATS_ATOMIC_BIT_KIND(descriptor);
        result = kind == 0 ? a | b : kind == 1 ? a & ~b : a ^ b;
        flags = (before & ~CC_C) | (a & b ? CC_C : 0);
        change_flags = false;
        break;
    }
    case LATS_ATOMIC_OR:
        result = a | b;
        break;
    case LATS_ATOMIC_AND:
        result = a & b;
        break;
    case LATS_ATOMIC_XOR:
        result = a ^ b;
        break;
    case LATS_ATOMIC_NOT:
        result = ~a & mask;
        change_flags = false;
        break;
    case LATS_ATOMIC_XCHG:
        result = b;
        change_flags = false;
        break;
    default:
        g_assert_not_reached();
    }
    if (change_flags) {
        bool logical = operation == LATS_ATOMIC_AND ||
                       operation == LATS_ATOMIC_OR ||
                       operation == LATS_ATOMIC_XOR;
        flags = arithmetic_flags(a, b, result, bits, subtract);
        if (logical) {
            flags &= ~(CC_A | CC_O);
        }
        if (carry) {
            flags |= CC_C;
        }
        if (operation == LATS_ATOMIC_INC || operation == LATS_ATOMIC_DEC) {
            flags = (flags & ~CC_C) | (before & CC_C);
        }
        flags |= before & ~(CC_C | CC_P | CC_A | CC_Z | CC_S | CC_O);
    }
    *result_flags = flags;
    return operation == LATS_ATOMIC_CMPXCHG
               ? (accumulator == previous ? source : previous)
               : result;
}

static void atomic_commit(CPUX86State *env, uint32_t descriptor,
                          uint64_t previous, uint64_t flags)
{
    unsigned bits = LATS_ATOMIC_BYTES(descriptor) * 8;
    unsigned operation = LATS_ATOMIC_OPERATION(descriptor);
    uint64_t mask = UINT64_MAX >> (64 - bits);
    if (LATS_ATOMIC_PAIR(descriptor)) {
        if (!(flags & CC_Z)) {
            env->regs[R_EAX] = (uint32_t)previous;
            env->regs[R_EDX] = (uint32_t)(previous >> 32);
        }
    } else if (operation == LATS_ATOMIC_CMPXCHG &&
               (env->regs[R_EAX] & mask) != previous) {
        env->regs[R_EAX] =
            partial_register(env->regs[R_EAX], previous, bits, 0);
    }
    if (operation == LATS_ATOMIC_XADD || operation == LATS_ATOMIC_XCHG) {
        unsigned reg = LATS_ATOMIC_REGISTER(descriptor);
        env->regs[reg] = partial_register(env->regs[reg], previous, bits,
                                          LATS_ATOMIC_HIGH_BYTE(descriptor));
    }
    env->eflags = flags;
    env->cc_src = flags & (CC_C | CC_P | CC_A | CC_Z | CC_S | CC_O);
    env->cc_op = CC_OP_EFLAGS;
#ifdef CONFIG_LATX_OPT_EFLAGS
    __lbt_mtflag_all(env->cc_src);
#endif
    env->eip += LATS_ATOMIC_LENGTH(descriptor);
    env->aot_native_atomic.completed++;
}

/*
 * Called only from cpu_exec_step_atomic's existing exclusive section.
 * All permission checks precede the first write. RAM pointers obtained by
 * the probes remain stable while other guest CPUs cannot execute.
 */
bool latx_aot_atomic_complete(CPUX86State *env)
{
    LatxNativeAtomicRequest *request = &env->aot_native_atomic;
    uint32_t descriptor = request->descriptor;
    if (!descriptor) {
        return false;
    }
    request->descriptor = 0;
    env->native_recovered = true;
    CPUState *cpu = env_cpu(env);
    g_assert(cpu_in_exclusive_context(cpu));
    unsigned bytes = LATS_ATOMIC_BYTES(descriptor);
    uint64_t address = request->address;
    if (address > UINT64_MAX - (bytes - 1)) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, 0);
    }
    uint8_t *parts[2];
    unsigned lengths[2] = {
        MIN(bytes, TARGET_PAGE_SIZE - (address & ~TARGET_PAGE_MASK)), 0};
    lengths[1] = bytes - lengths[0];
    int mmu = cpu_mmu_index(env, false);
    for (unsigned n = 0; n < 2 && lengths[n]; n++) {
        uint64_t pc = address + (n ? lengths[0] : 0);
        if (!probe_write(env, pc, lengths[n], mmu, 0)) {
            error_report("LLVM atomic operations require RAM");
            abort();
        }
    }
    for (unsigned n = 0; n < 2 && lengths[n]; n++) {
        parts[n] =
            probe_read(env, address + (n ? lengths[0] : 0), lengths[n], mmu, 0);
        if (!parts[n]) {
            error_report("LLVM atomic operations require readable RAM");
            abort();
        }
    }
    /*
     * probe_write dirties a page using only its first byte. Invalidate the
     * complete write range as a TB may start inside this atomic operand.
     */
    for (unsigned n = 0; n < 2 && lengths[n]; n++) {
        ram_addr_t physical = qemu_ram_addr_from_host_nofail(parts[n]);
        tb_invalidate_phys_range(physical, physical + lengths[n] - 1);
    }
    uint64_t previous = 0;
    memcpy(&previous, parts[0], lengths[0]);
    if (lengths[1]) {
        memcpy((uint8_t *)&previous + lengths[0], parts[1], lengths[1]);
    }
    uint64_t flags;
    uint64_t stored = atomic_value(env, descriptor, previous, env->eflags, &flags);
    /*
     * There are no faulting helpers after this point. The exclusive section
     * makes even a two-page RAM write indivisible to other guest CPUs.
     */
    memcpy(parts[0], &stored, lengths[0]);
    if (lengths[1]) {
        memcpy(parts[1], (uint8_t *)&stored + lengths[0], lengths[1]);
    }
    atomic_commit(env, descriptor, previous, flags);
    env->native_recovered = false;
    return true;
}

/*
 * Use the same permission, alignment and SMC handling as ordinary TCG.
 * atomic_mmu_lookup subtracts GETPC_ADJ; the effective recovery PC is zero
 * because the returning entry has already committed the guest state.
 */
static uint64_t atomic_access(CPUX86State *env, unsigned bytes,
                              unsigned operation, uint64_t value,
                              uint64_t compare)
{
    uint64_t address = env->aot_native_atomic.address;
    unsigned mmu = cpu_mmu_index(env, false);
#define ATOMIC_ACCESS(name, ...)                                               \
    do {                                                                       \
        switch (bytes) {                                                       \
        case 1:                                                                \
            return cpu_atomic_##name##b_mmu(env, address, __VA_ARGS__,         \
                                            make_memop_idx(MO_8, mmu),         \
                                            GETPC_ADJ);                        \
        case 2:                                                                \
            return cpu_atomic_##name##w_le_mmu(env, address, __VA_ARGS__,      \
                                               make_memop_idx(MO_LEUW, mmu),   \
                                               GETPC_ADJ);                     \
        case 4:                                                                \
            return cpu_atomic_##name##l_le_mmu(env, address, __VA_ARGS__,      \
                                               make_memop_idx(MO_LEUL, mmu),   \
                                               GETPC_ADJ);                     \
        case 8:                                                                \
            return cpu_atomic_##name##q_le_mmu(env, address, __VA_ARGS__,      \
                                               make_memop_idx(MO_LEUQ, mmu),   \
                                               GETPC_ADJ);                     \
        default:                                                               \
            g_assert_not_reached();                                            \
        }                                                                      \
    } while (0)
    switch (operation) {
    case LATS_ATOMIC_ADD:
        ATOMIC_ACCESS(fetch_add, value);
    case LATS_ATOMIC_SUB:
        ATOMIC_ACCESS(fetch_add, -value);
    case LATS_ATOMIC_AND:
        ATOMIC_ACCESS(fetch_and, value);
    case LATS_ATOMIC_OR:
        ATOMIC_ACCESS(fetch_or, value);
    case LATS_ATOMIC_XOR:
        ATOMIC_ACCESS(fetch_xor, value);
    case LATS_ATOMIC_XCHG:
        ATOMIC_ACCESS(xchg, value);
    case LATS_ATOMIC_CMPXCHG:
        ATOMIC_ACCESS(cmpxchg, compare, value);
    default:
        g_assert_not_reached();
    }
#undef ATOMIC_ACCESS
}

void latx_helper_aot_atomic(CPUX86State *env, unsigned descriptor)
{
    LatxNativeAtomicRequest *request = &env->aot_native_atomic;
    unsigned simd = env->is_fcsr_simd;
    /* The helper prologue saved arithmetic flags in cc_src. cpu_exec keeps
     * them out of eflags, and C code may already have changed physical LBT. */
    uint64_t before = env->eflags | env->cc_src | (env->df & DF_MASK);
    descriptor &= ~LATS_ATOMIC_RETURN;
    g_assert(!request->descriptor && LATS_ATOMIC_LENGTH(descriptor));
    request->descriptor = descriptor;
    env->native_recovered = true;
    env->is_fcsr_simd = 0;
    unsigned bytes = LATS_ATOMIC_BYTES(descriptor);
    unsigned operation = LATS_ATOMIC_OPERATION(descriptor), access = operation;
    uint64_t source = request->value, mask = UINT64_MAX >> (64 - bytes * 8);
    uint64_t previous;

    switch (operation) {
    case LATS_ATOMIC_BIT:
        access = LATS_ATOMIC_BIT_KIND(descriptor) == 0   ? LATS_ATOMIC_OR
                 : LATS_ATOMIC_BIT_KIND(descriptor) == 1 ? LATS_ATOMIC_AND
                                                         : LATS_ATOMIC_XOR;
        if (LATS_ATOMIC_BIT_KIND(descriptor) == 1) {
            source = ~source;
        }
        break;
    case LATS_ATOMIC_INC:
        source = 1; /* fall through */
    case LATS_ATOMIC_XADD:
        access = LATS_ATOMIC_ADD;
        break;
    case LATS_ATOMIC_DEC:
        source = 1;
        access = LATS_ATOMIC_SUB;
        break;
    case LATS_ATOMIC_ADC:
        source += !!(before & CC_C);
        access = LATS_ATOMIC_ADD;
        break;
    case LATS_ATOMIC_SBB:
        source += !!(before & CC_C);
        access = LATS_ATOMIC_SUB;
        break;
    case LATS_ATOMIC_NOT:
        source = mask;
        access = LATS_ATOMIC_XOR;
        break;
    default:
        break;
    }
    if (operation == LATS_ATOMIC_NEG) {
        previous = atomic_access(env, bytes, LATS_ATOMIC_ADD, 0, 0);
        for (unsigned retry = 0;; retry++) {
            uint64_t observed = atomic_access(env, bytes, LATS_ATOMIC_CMPXCHG,
                                              -previous & mask, previous);
            if (observed == previous) {
                break;
            }
            if (retry == 15) {
                cpu_loop_exit_atomic(env_cpu(env), 0);
            }
            previous = observed;
        }
    } else {
        uint64_t compare =
            LATS_ATOMIC_PAIR(descriptor)
                ? (uint32_t)env->regs[R_EAX] |
                      ((uint64_t)(uint32_t)env->regs[R_EDX] << 32)
                : env->regs[R_EAX] & mask;
        previous = atomic_access(env, bytes, access, source, compare);
    }
    uint64_t flags;
    atomic_value(env, descriptor, previous, before, &flags);
    atomic_commit(env, descriptor, previous, flags);
    /* Unlike the serial retry, this path remains inside cpu_exec. */
    env->eflags &= ~(CC_C | CC_P | CC_A | CC_Z | CC_S | CC_O | DF_MASK);
    request->descriptor = 0;
    /*
     * Invalidation can affect this body or any suspended LLVM caller.
     * Check before the returning epilogue consumes the RAS notification.
     */
    if (qatomic_read(&env->ras_tp.reset_pending)) {
        cpu_loop_exit_noexc(env_cpu(env));
    }
    env->is_fcsr_simd = simd;
    env->native_recovered = false;
}
