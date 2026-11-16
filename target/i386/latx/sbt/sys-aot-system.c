/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "exec/helper-proto.h"
#include "accel/tcg/tb-context.h"
#include "latx-helper.h"
#include "lats/aot-system.h"

void latx_helper_aot_system(CPUX86State *env, unsigned descriptor,
                            uint64_t operand)
{
    unsigned simd = env->is_fcsr_simd;
    unsigned kind = LATS_SYSTEM_KIND(descriptor),
             destination = LATS_SYSTEM_DEST(descriptor);
    unsigned mtcc = curr_mtcc(env);
    unsigned invalidations = qatomic_read(&tb_ctx.tb_phys_invalidate_count);
    unsigned flushes = qatomic_read(&tb_ctx.tb_flush_count);
    unsigned partial_flushes = qatomic_read(&tb_ctx.tb_flush_count_mtcc[mtcc]);
    bool writes =
        kind == LATS_SYSTEM_CMPXCHG8B || kind == LATS_SYSTEM_CMPXCHG16B;
    env->native_recovered = true;
    env->is_fcsr_simd = 0;
    if (kind <= LATS_SYSTEM_PCMPESTRM) {
        ZMMReg memory = {0};
        void *source = &env->xmm_regs[LATS_SYSTEM_SOURCE(descriptor)];
        if (LATS_SYSTEM_MEMORY(descriptor)) {
            memory.ZMM_Q(0) = cpu_ldq_data(env, operand);
            memory.ZMM_Q(1) = cpu_ldq_data(env, operand + 8);
            source = &memory;
        }
        void *target = &env->xmm_regs[destination];
        unsigned control = LATS_SYSTEM_CONTROL(descriptor);
        switch (kind) {
        case LATS_SYSTEM_PCMPISTRI:
            helper_pcmpistri_xmm(env, target, source, control);
            break;
        case LATS_SYSTEM_PCMPISTRM:
            helper_pcmpistrm_xmm(env, target, source, control);
            break;
        case LATS_SYSTEM_PCMPESTRI:
            helper_pcmpestri_xmm(env, target, source, control);
            break;
        case LATS_SYSTEM_PCMPESTRM:
            helper_pcmpestrm_xmm(env, target, source, control);
            break;
        }
    } else {
        switch (kind) {
        case LATS_SYSTEM_LAR:
        case LATS_SYSTEM_LSL: {
            uint64_t value = kind == LATS_SYSTEM_LAR ? helper_lar(env, operand)
                                                     : helper_lsl(env, operand);
            if (env->cc_src & CC_Z) {
                unsigned width = LATS_SYSTEM_WIDTH(descriptor);
                env->regs[destination] =
                    width == 0 ? (env->regs[destination] & ~UINT64_C(0xffff)) |
                                     (uint16_t)value
                    : width == 1 ? (uint32_t)value
                                 : value;
            }
            break;
        }
        case LATS_SYSTEM_VERR:
            helper_verr(env, operand);
            break;
        case LATS_SYSTEM_VERW:
            helper_verw(env, operand);
            break;
        case LATS_SYSTEM_CMPXCHG8B:
            helper_latx_cmpxchg8b_unlocked(env, operand);
            break;
        case LATS_SYSTEM_CMPXCHG16B:
            helper_latx_cmpxchg16b_unlocked(env, operand);
            break;
        default:
            g_assert_not_reached();
        }
    }
    if ((writes &&
         invalidations != qatomic_read(&tb_ctx.tb_phys_invalidate_count)) ||
        flushes != qatomic_read(&tb_ctx.tb_flush_count) ||
        partial_flushes != qatomic_read(&tb_ctx.tb_flush_count_mtcc[mtcc]) ||
        qatomic_read(&env->ras_tp.reset_pending)) {
        env->eip += LATS_SYSTEM_LENGTH(descriptor);
        cpu_loop_exit_noexc(env_cpu(env));
    }
    env->is_fcsr_simd = simd;
    env->native_recovered = false;
}
