/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "sysemu/cpus.h"
#include "sysemu/tcg.h"
#include "sysemu/cpu-timers.h"
#include "exec/exec-all.h"
#include "qemu/plugin.h"
#include "internal.h"

#include "hamt-internal.h"
#include "latx/include/latx-config.h"
#include "tcg/tcg-rrtrace.h"

bool tcg_allowed;

/* exit the current TB, but without causing any exception to be raised */
void cpu_loop_exit_noexc(CPUState *cpu)
{
    cpu->exception_index = -1;
    cpu_loop_exit(cpu);
}

void cpu_loop_exit(CPUState *cpu)
{
    /* Undo the setting in cpu_tb_exec.  */
    cpu->can_do_io = 1;
#if !defined(CONFIG_USER_ONLY)
    if (icount_enabled() && qemu_rrtrace_enabled()) {
        rrtrace_tb_after_excep(cpu);
    }
#endif
    if (qemu_tcg_mode_2_lat()) {
        latxs_fix_after_excp_or_int(cpu);
    }
    /* Undo any setting in generated code.  */
    qemu_plugin_disable_mem_helpers(cpu);
#ifdef CONFIG_SOFTMMU
    CPUX86State *env = cpu->env_ptr;
    if (env->in_hamt_exception) {
        /*
         * CRMD recovery differs per exception;
         * 'ertn' is appropriate.
         */
        restore_prmd(cpu->cpu_index);
        loongarch_ertn();
        env->in_hamt_exception = false;
    }
#endif
    siglongjmp(cpu->jmp_env, 1);
}

void cpu_loop_exit_restore(CPUState *cpu, uintptr_t pc)
{
    if (pc) {
#if defined(CONFIG_LATX_TCG_MODE) || !defined(CONFIG_SOFTMMU)
        cpu_restore_state(cpu, pc);
#else
        bool restored = cpu_restore_state(cpu, pc);

        if (restored) {
            cpu_loop_exit_recover_ptn_eflag(cpu, pc);
        }
#endif
    }
    cpu_loop_exit(cpu);
}

void cpu_loop_exit_atomic(CPUState *cpu, uintptr_t pc)
{
    /* Prevent looping if already executing in a serial context. */
    g_assert(!cpu_in_serial_context(cpu));
    cpu->exception_index = EXCP_ATOMIC;
    cpu_loop_exit_restore(cpu, pc);
}
