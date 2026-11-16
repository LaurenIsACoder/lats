/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "tcg/helper-tcg.h"
#include "lats/aot-cpu-query.h"
#include "latx-options.h"
#include "latx-helper.h"

void latx_helper_aot_cpu_query(CPUX86State *env, unsigned descriptor)
{
    unsigned simd = env->is_fcsr_simd;
    uint64_t value;

    /* The returning entry has already committed the complete guest state.
     * A helper exception must use it rather than C's physical registers. */
    env->native_recovered = true;
    env->is_fcsr_simd = 0;
    switch (LATS_CPU_QUERY_KIND(descriptor)) {
    case LATS_CPU_QUERY_CPUID:
        helper_cpuid(env);
        break;
    case LATS_CPU_QUERY_RDTSC:
        helper_rdtsc(env);
        break;
    case LATS_CPU_QUERY_XGETBV:
        if (!(env->features[FEAT_1_ECX] & CPUID_EXT_XSAVE)) {
            raise_exception(env, EXCP06_ILLOP);
        }
        value = helper_xgetbv(env, (uint32_t)env->regs[R_ECX]);
        env->regs[R_EAX] = (uint32_t)value;
        env->regs[R_EDX] = (uint32_t)(value >> 32);
        break;
    case LATS_CPU_QUERY_PAUSE:
        if (!option_pause) {
            helper_pause(env, LATS_CPU_QUERY_LENGTH(descriptor));
        }
        break;
    default:
        g_assert_not_reached();
    }
    env->is_fcsr_simd = simd;
    env->native_recovered = false;
}
