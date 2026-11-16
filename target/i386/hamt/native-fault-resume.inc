/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Suspend a live LLVM frame across guest demand paging or interrupts. */
#include "accel/tcg/tb-context.h"

#define HAMT_NATIVE_FRAME_BYTES 2128
#define HAMT_NATIVE_SCRATCH_BYTES (64 * 1024)
#define HAMT_NATIVE_STACK_LIMIT (8 * 1024 * 1024)

typedef struct HamtNativeFault {
    uint64_t guest_pc, guest_sp, cr3, host_pc;
    bool asynchronous, resume_ready;
    uintptr_t outer_sp, stack_begin;
    size_t stack_bytes;
    uint8_t *stack_copy;
    uint8_t *scratch;
    LatxRasTPState ras;
    uint8_t frame[HAMT_NATIVE_FRAME_BYTES] __attribute__((aligned(32)));
    TranslationBlock *owner;
    uint32_t mtcc, full_flush, partial_flush;
    target_ulong regs[CPU_NB_REGS];
    uint8_t xmm[16][16];
    uint32_t flags, mxcsr;
    uint64_t fs_base, gs_base;
#ifdef CONFIG_LATX_OPT_NOFP
    int nofp;
#endif
} HamtNativeFault;

void hamt_native_fault_clear(CPUX86State *env)
{
    HamtNativeFault *fault = env->aot_native_fault;
    if (fault) {
        g_free(fault->stack_copy);
        g_free(fault->scratch);
        free(fault);
        env->aot_native_fault = NULL;
    }
    env->aot_native_fault_active = 0;
}

void hamt_fault_switch_stack(HamtNativeFault *, CPUX86State *, void *)
    __attribute__((noreturn));
void hamt_fault_restore_frame(void *) __attribute__((noreturn));
void hamt_fault_resume_on_stack(HamtNativeFault *, CPUX86State *)
    __attribute__((noreturn));

void hamt_fault_resume_on_stack(HamtNativeFault *fault, CPUX86State *env)
{
    /* The active C stack is in scratch, so restoring the original stack is safe. */
    memcpy((void *)fault->stack_begin, fault->stack_copy, fault->stack_bytes);
    /* Restore the prediction stack without undoing callbacks completed while
     * the guest kernel ran, or losing a pending invalidation request. */
    env->ras_tp.top = fault->ras.top;
    env->ras_tp.sentinel = fault->ras.sentinel;
    memcpy(env->ras_tp.stack, fault->ras.stack, sizeof(env->ras_tp.stack));
    env->ras_tp.native_tp = fault->ras.native_tp;
    env->ras_tp.mtcc_id = fault->ras.mtcc_id;
    env->ras_tp.initialized = fault->ras.initialized;
#ifdef CONFIG_LATX_OPT_NOFP
    env->nofp = fault->nofp;
#endif
    env->aot_native_outer_sp = fault->outer_sp;
    env->aot_native_fault_active = 0;
    if (hamt_mode_dmw() && !hamt_disable_asid() && option_pcid_asid) {
        uint64_t *frame = (uint64_t *)fault->frame;
        frame[PT_ASID_INDEX] =
            (frame[PT_ASID_INDEX] & ~UINT64_C(0x3ff)) |
            (env->hamt_guest_asid & 0x3ffU);
    }
    hamt_fault_restore_frame(fault->frame);
}

static void hamt_native_resume_check_code(CPUX86State *env,
                                          HamtNativeFault *fault)
{
    if (fault->mtcc != curr_mtcc(env) ||
        fault->full_flush != qatomic_read(&tb_ctx.tb_flush_count) ||
        fault->partial_flush !=
            qatomic_read(&tb_ctx.tb_flush_count_mtcc[fault->mtcc])) {
        env->aot_native_fault_active = 0;
        error_report("LLVM demand-page resume: translated-code region was flushed");
        abort();
    }
    TranslationBlock *tb = tcg_tb_lookup(fault->host_pc);
    const AOTNativeRecord *record = hamt_native_fault_record(fault->host_pc);
    if (!tb || (tb_cflags(tb) & CF_INVALID) || !record ||
        record->tb != fault->owner || (tb_cflags(record->tb) & CF_INVALID)) {
        error_report("LLVM page fault resume: compiled code invalidated");
        abort();
    }
}

static HamtNativeFault *hamt_native_resume_check(CPUX86State *env, bool iret)
{
    HamtNativeFault *fault = env->aot_native_fault;
    if (!fault || !env->aot_native_fault_active) {
        return NULL;
    }
    if (env->cr[3] != fault->cr3 || (env->hflags & HF_CPL_MASK) != 3 ||
        env->regs[R_ESP] != fault->guest_sp) {
        return NULL;
    }
    /* Signals and changed return contexts need precise recovery. Do not
     * retain an active snapshot and accidentally match a later return. */
    uint32_t regs_changed = 0, xmm_changed = 0;
    for (unsigned i = 0; i < CPU_NB_REGS; i++) {
        regs_changed |= (env->regs[i] != fault->regs[i]) << i;
    }
    for (unsigned i = 0; i < 16; i++) {
        xmm_changed |= (memcmp(&env->xmm_regs[i]._b_ZMMReg[0],
                               fault->xmm[i], 16) != 0) << i;
    }
    /* IRET has loaded CC_SRC and DF from the saved user flags. Compare
     * those architectural values rather than a helper's live LBT scratch. */
    uint32_t flags = iret ? env->cc_src | (env->df & DF_MASK) :
                            cpu_compute_eflags(env);
    bool changed = env->eip != fault->guest_pc || regs_changed || xmm_changed ||
        ((flags ^ fault->flags) &
         (CC_C | CC_P | CC_A | CC_Z | CC_S | CC_O | DF_MASK)) ||
        env->mxcsr != fault->mxcsr ||
        env->segs[R_FS].base != fault->fs_base || env->segs[R_GS].base != fault->gs_base;
    if (changed) {
        env->aot_native_fault_active = 0;
        error_report("LLVM demand-page resume: changed guest context "
                     "(PC %" PRIx64 "/%" PRIx64 ", GPR mask %x, XMM mask %x, "
                     "flags %x/%x, MXCSR %x/%x); precise recovery required",
                     env->eip, fault->guest_pc, regs_changed, xmm_changed,
                     flags, fault->flags, env->mxcsr, fault->mxcsr);
        abort();
    }
    hamt_native_resume_check_code(env, fault);
    return fault;
}

void hamt_native_resume_iret(CPUX86State *env)
{
    HamtNativeFault *fault = hamt_native_resume_check(env, true);
    if (fault) {
        CPUState *cpu = env_cpu(env);
        uint32_t cflags = cpu->cflags_next_tb == -1 ? curr_cflags(cpu) :
                                                    cpu->cflags_next_tb;

        if (cpu->curr_exec_tb &&
            env->aot_native_outer_sp == fault->outer_sp &&
            !(cflags & (CF_SINGLE_STEP | CF_USE_ICOUNT |
                        CF_NO_GOTO_TB | CF_NO_GOTO_PTR)) &&
            QTAILQ_EMPTY(&cpu->breakpoints) &&
            QTAILQ_EMPTY(&cpu->watchpoints) &&
            !qatomic_read(&cpu->exit_request) &&
            !qatomic_read(&cpu->interrupt_request) &&
            qatomic_read(&cpu_neg(cpu)->icount_decr.u16.high) == 0) {
            /* IRET still owns a live translator return frame. Preserve it
             * and restore only the suspended LLVM stack below that frame. */
            env->hflags &= ~HF_FORCE_TB_MODE;
            cpu->curr_exec_tb = fault->owner;
            hamt_fault_switch_stack(fault, env,
                (void *)(((uintptr_t)fault->scratch + HAMT_NATIVE_SCRATCH_BYTES) &
                         ~(uintptr_t)15));
        }
        fault->resume_ready = true;
        env->native_recovered = true;
        cpu_loop_exit_noexc(env_cpu(env));
    }
    if (env->aot_native_fault_active && (env->hflags & HF_CPL_MASK) == 3) {
        /* IRET has already committed SIMD state before entering C. */
        env->native_recovered = true;
        cpu_loop_exit_noexc(env_cpu(env));
    }
}

TranslationBlock *hamt_native_resume_tb(CPUX86State *env)
{
    HamtNativeFault *fault = env->aot_native_fault;
    if (!env->aot_native_fault_active ||
        (!fault->resume_ready && !fault->asynchronous) ||
        env->cr[3] != fault->cr3 || (env->hflags & HF_CPL_MASK) != 3 ||
        env->regs[R_ESP] != fault->guest_sp) {
        return NULL;
    }
    if (!fault->resume_ready && !hamt_native_resume_check(env, false)) {
        return NULL;
    }
    hamt_native_resume_check_code(env, fault);
    fault->resume_ready = true;
    env->hflags &= ~HF_FORCE_TB_MODE;
    return fault->owner;
}

static void __attribute__((used, noreturn))
hamt_native_resume_from_entry(CPUX86State *env)
{
    HamtNativeFault *fault = env->aot_native_fault;
    g_assert(fault && env->aot_native_fault_active && fault->resume_ready);
    hamt_native_resume_check_code(env, fault);
    /* The normal translator prologue owns a fresh host return frame.
     * Keep it intact and restore only the LLVM frames below it. Internal
     * stack pointers cannot be relocated when the entry address differs. */
    if (env->aot_native_outer_sp != fault->outer_sp) {
        error_report("LLVM resume: translator stack address changed");
        abort();
    }
    hamt_fault_switch_stack(fault, env,
        (void *)(((uintptr_t)fault->scratch + HAMT_NATIVE_SCRATCH_BYTES) &
                 ~(uintptr_t)15));
}

void __attribute__((noinline, noreturn)) hamt_native_resume_entry(void)
{
    /* Entered after the translator prologue, with env in s0 and a valid
     * saved C frame. Restore host TLS before entering the C helper. */
    asm volatile(
        "or $a0, $s0, $zero\n"
        "csrwr $zero, 0x18\n"
        "lu52i.d $a0, $a0, 0\n"
        "ldptr.d $tp, $a0, %0\n"
        "lu52i.d $sp, $sp, 0\n"
        "b hamt_native_resume_from_entry\n"
        : : "i"(offsetof(CPUX86State, ras_tp.native_tp)) : "memory");
    __builtin_unreachable();
}

static HamtNativeFault *hamt_native_suspend_context(CPUX86State *env,
                                                   uintptr_t epc, bool async,
                                                   uint64_t *gprs)
{
    const AOTNativeRecord *record = hamt_native_fault_record(epc);
    g_assert(record && (env->hflags & HF_CPL_MASK) == 3);
    uintptr_t begin = DMW_OUT(gprs[3]);
    uintptr_t end = DMW_OUT(env->aot_native_outer_sp);
    if (begin > end || end - begin > HAMT_NATIVE_STACK_LIMIT) {
        error_report("LLVM page fault resume: invalid host stack range");
        abort();
    }
    HamtNativeFault *fault = env->aot_native_fault;
    if (!fault) {
        if (posix_memalign((void **)&fault, 32, sizeof(*fault))) {
            error_report("LLVM page fault resume: context allocation failed");
            abort();
        }
        memset(fault, 0, sizeof(*fault));
        fault->scratch = g_malloc(HAMT_NATIVE_SCRATCH_BYTES);
        env->aot_native_fault = fault;
    }
    if (env->aot_native_fault_active) {
        error_report("LLVM page fault resume: another fault is suspended");
        abort();
    }
    env->aot_native_fault_active = 1;
    fault->host_pc = epc;
    fault->asynchronous = async;
    fault->resume_ready = false;
    fault->guest_pc = record->tb->pc;
    fault->guest_sp = gprs[28];
    fault->cr3 = env->cr[3];
    fault->outer_sp = env->aot_native_outer_sp;
    fault->stack_begin = begin;
    fault->stack_bytes = end - begin;
    fault->stack_copy = g_realloc(fault->stack_copy, MAX(fault->stack_bytes, 1));
    if ((void *)gprs != fault->frame) {
        memcpy(fault->frame, gprs, sizeof(fault->frame));
    }
    memcpy(fault->stack_copy, (const void *)begin, fault->stack_bytes);
    fault->ras = env->ras_tp;
#ifdef CONFIG_LATX_OPT_NOFP
    fault->nofp = env->nofp;
#endif

    /* No exact x86 PC is needed for the normal demand-page case. The
     * register image supplied to the guest kernel is approximate. */
    hamt_save_from_native_frame(env, (void *)fault->frame);
    /* NOFP keeps these values in physical registers. Use the captured
     * exception frame before C helpers or the guest kernel can change them. */
    for (unsigned i = 0; i < 16; i++) {
        memcpy(&env->xmm_regs[i]._b_ZMMReg[0],
               fault->frame + NATIVE_XR_OFFSET + (16 + i) * 32, 16);
    }
#ifdef CONFIG_LATX_OPT_NOFP
    bool sync_fprs = env->nofp != 0;
#else
    bool sync_fprs = !option_soft_fpu_fast || !env->mode_fpu;
#endif
    if (sync_fprs) {
        for (unsigned i = 0; i < 8; i++) {
            memcpy(&env->fpregs[i].d.low,
                   fault->frame + NATIVE_XR_OFFSET + i * 32, 8);
        }
#ifdef CONFIG_LATX_OPT_NOFP
        env->nofp = 0;
#endif
    }
    fault->owner = record->tb;
    fault->mtcc = curr_mtcc(env);
    fault->full_flush = qatomic_read(&tb_ctx.tb_flush_count);
    fault->partial_flush = qatomic_read(&tb_ctx.tb_flush_count_mtcc[fault->mtcc]);
    memcpy(fault->regs, env->regs, sizeof(fault->regs));
    for (unsigned i = 0; i < 16; i++) {
        memcpy(fault->xmm[i], &env->xmm_regs[i]._b_ZMMReg[0], 16);
    }
    fault->flags = cpu_compute_eflags(env);
    if (option_soft_fpu) {
        uint32_t fcsr;
        memcpy(&fcsr, fault->frame + NATIVE_FCSR_OFFSET, sizeof(fcsr));
        /* The kernel saves physical FCSR status using the STMXCSR mapping.
         * Match that image before checking the context returned by IRET. */
        env->mxcsr = (env->mxcsr & ~UINT32_C(0x3d)) |
            (((fcsr >> 16) & 1) << 5) | (((fcsr >> 17) & 1) << 4) |
            (((fcsr >> 18) & 1) << 3) | (((fcsr >> 19) & 1) << 2) |
            ((fcsr >> 20) & 1);
    }
    fault->mxcsr = env->mxcsr;
    fault->fs_base = env->segs[R_FS].base;
    fault->gs_base = env->segs[R_GS].base;
    env->eip = fault->guest_pc;
    return fault;
}

static bool hamt_native_suspend_page_fault(CPUX86State *env, uintptr_t epc,
                                          uint64_t addr, int error_code)
{
    const AOTNativeRecord *record = hamt_native_fault_record(epc);
    if (!record || !record->recover ||
        ((const NativeRecoverHeader *)record->recover)->state_bytes ||
        ((const NativeRecoverHeader *)record->recover)->points ||
        (env->hflags & HF_CPL_MASK) != 3 || !option_rastoptp || !hamt_mode_dmw()) {
        return false;
    }
    CPUState *cpu = env_cpu(env);
    int cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;
    uint64_t *gprs = (void *)hamt_data_area[cpuid];
    hamt_native_suspend_context(env, epc, false, gprs);
    env->cr[2] = addr;
    lats_do_interrupt64(env, EXCP0E_PAGE, 0, error_code, 0, 0);
    uint64_t target = GET_SC_TABLE(0, intb_lookup);
    if (!target) {
        target = GET_SC_TABLE(curr_mtcc(env), cs_native_to_bt_ret_0);
    }
    gprs[28] = env->regs[R_ESP];
    gprs[PT_ERA_INDEX] = DMW_IN(target);
    gprs[5] = env->eip;
    latxs_ras_tp_redirect(env, gprs);
    hamt_restore_to_native(env, cpuid);
    return true;
}

/* KVM has stopped before filling the private snapshot frame. The capture
 * stub runs with host interrupts disabled until all physical state is saved. */
void *hamt_native_async_frame(CPUX86State *env, uintptr_t pc, uintptr_t sp)
{
    if (option_int_opt != IRQ_CHECK_ASYNC_NOTIFY || !option_rastoptp ||
        !qemu_tcg_mttcg_enabled() ||
        !hamt_mode_dmw() || !(cpuinfo & CPUINFO_LASX) ||
        !current_machine || current_machine->smp.max_cpus != 1 ||
        env->aot_native_fault_active || env->in_hamt_exception ||
        (env->hflags & HF_CPL_MASK) != 3 || !hamt_native_fault_record(pc)) {
        return NULL;
    }
    uintptr_t begin = DMW_OUT(sp), end = DMW_OUT(env->aot_native_outer_sp);
    if (begin > end || end - begin > HAMT_NATIVE_STACK_LIMIT) {
        return NULL;
    }
    HamtNativeFault *fault = env->aot_native_fault;
    if (!fault) {
        if (posix_memalign((void **)&fault, 32, sizeof(*fault))) {
            error_report("LLVM asynchronous exit: context allocation failed");
            abort();
        }
        memset(fault, 0, sizeof(*fault));
        fault->scratch = g_malloc(HAMT_NATIVE_SCRATCH_BYTES);
        env->aot_native_fault = fault;
    }
    fault->stack_copy = g_realloc(fault->stack_copy, MAX(end - begin, 1));
    return fault->frame;
}

static void __attribute__((used, noreturn))
hamt_native_async_suspend(uint64_t *frame, CPUX86State *env)
{
    hamt_native_suspend_context(env, DMW_OUT(frame[PT_ERA_INDEX]), true, frame);
    CPUState *cpu = env_cpu(env);
    unsigned cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;
    ((uint64_t *)hamt_data_area[cpuid])[PT_PRMD_INDEX] = frame[PT_PRMD_INDEX];
    /* The dispatcher must not overwrite the saved image from C temporaries. */
    env->is_fcsr_simd = 0;
    env->native_recovered = true;
    env->in_hamt_exception = true;
    /* The owner clears its active prediction depth without publishing a
     * reset that would remain pending on the immediate-resume path. */
    env->ras_tp.top = 0;
    cpu_loop_exit_noexc(env_cpu(env));
}

__asm__(
    ".text\n.p2align 2\n"
    ".globl hamt_native_async_entry\n"
    ".type hamt_native_async_entry,@function\n"
    "hamt_native_async_entry:\n"
    "addi.d $a2, $a0, 1024\n"
    "xvst $xr0, $a2, 0\n"
    "xvst $xr1, $a2, 32\n"
    "xvst $xr2, $a2, 64\n"
    "xvst $xr3, $a2, 96\n"
    "xvst $xr4, $a2, 128\n"
    "xvst $xr5, $a2, 160\n"
    "xvst $xr6, $a2, 192\n"
    "xvst $xr7, $a2, 224\n"
    "xvst $xr8, $a2, 256\n"
    "xvst $xr9, $a2, 288\n"
    "xvst $xr10, $a2, 320\n"
    "xvst $xr11, $a2, 352\n"
    "xvst $xr12, $a2, 384\n"
    "xvst $xr13, $a2, 416\n"
    "xvst $xr14, $a2, 448\n"
    "xvst $xr15, $a2, 480\n"
    "xvst $xr16, $a2, 512\n"
    "xvst $xr17, $a2, 544\n"
    "xvst $xr18, $a2, 576\n"
    "xvst $xr19, $a2, 608\n"
    "xvst $xr20, $a2, 640\n"
    "xvst $xr21, $a2, 672\n"
    "xvst $xr22, $a2, 704\n"
    "xvst $xr23, $a2, 736\n"
    "xvst $xr24, $a2, 768\n"
    "xvst $xr25, $a2, 800\n"
    "xvst $xr26, $a2, 832\n"
    "xvst $xr27, $a2, 864\n"
    "xvst $xr28, $a2, 896\n"
    "xvst $xr29, $a2, 928\n"
    "xvst $xr30, $a2, 960\n"
    "xvst $xr31, $a2, 992\n"
    "movfcsr2gr $t0, $fcsr0\n"
    "st.w $t0, $a2, 1024\n"
    ".word 0x005cfc0c\n"
    "st.d $t0, $a2, 1032\n"
    "movcf2gr $t0, $fcc0\n"
    "st.d $t0, $a2, 1040\n"
    "movcf2gr $t0, $fcc1\n"
    "st.d $t0, $a2, 1048\n"
    "movcf2gr $t0, $fcc2\n"
    "st.d $t0, $a2, 1056\n"
    "movcf2gr $t0, $fcc3\n"
    "st.d $t0, $a2, 1064\n"
    "movcf2gr $t0, $fcc4\n"
    "st.d $t0, $a2, 1072\n"
    "movcf2gr $t0, $fcc5\n"
    "st.d $t0, $a2, 1080\n"
    "movcf2gr $t0, $fcc6\n"
    "st.d $t0, $a2, 1088\n"
    "movcf2gr $t0, $fcc7\n"
    "st.d $t0, $a2, 1096\n"
    "csrwr $zero, 0x18\n"
    "lu52i.d $a0, $a0, 0\n"
    "lu52i.d $a1, $a1, 0\n"
    "b hamt_native_async_suspend\n"
    ".size hamt_native_async_entry, .-hamt_native_async_entry\n"
);

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl hamt_fault_switch_stack\n"
    ".type hamt_fault_switch_stack,@function\n"
    "hamt_fault_switch_stack:\n"
    "or $sp, $a2, $zero\n"
    "b hamt_fault_resume_on_stack\n"
    ".size hamt_fault_switch_stack, .-hamt_fault_switch_stack\n"
    ".globl hamt_fault_restore_frame\n"
    ".type hamt_fault_restore_frame,@function\n"
    "hamt_fault_restore_frame:\n"
    /* IRET helpers run with host interrupts enabled. Protect ERA and PRMD
     * until ERTN restores the interrupted machine context. */
    "or $t2, $zero, $zero\n"
    "ori $t1, $zero, 4\n"
    "csrxchg $t2, $t1, 0\n"
    /* Keep fetching this code after the original ASID is restored. */
    "pcaddu12i $t1, 0\n"
    "addi.d $t1, $t1, 16\n"
    "lu52i.d $t1, $t1, -1792\n"
    "jirl $zero, $t1, 0\n"
    /* Restoring ASID changes ordinary-address mappings. Keep the saved
     * frame accessible via the same DMW alias used by HAMT exceptions. */
    "lu52i.d $t0, $a0, -1792\n"
    "addi.d $t1, $t0, 1024\n"
    "xvld $xr0, $t1, 0\n"
    "xvld $xr1, $t1, 32\n"
    "xvld $xr2, $t1, 64\n"
    "xvld $xr3, $t1, 96\n"
    "xvld $xr4, $t1, 128\n"
    "xvld $xr5, $t1, 160\n"
    "xvld $xr6, $t1, 192\n"
    "xvld $xr7, $t1, 224\n"
    "xvld $xr8, $t1, 256\n"
    "xvld $xr9, $t1, 288\n"
    "xvld $xr10, $t1, 320\n"
    "xvld $xr11, $t1, 352\n"
    "xvld $xr12, $t1, 384\n"
    "xvld $xr13, $t1, 416\n"
    "xvld $xr14, $t1, 448\n"
    "xvld $xr15, $t1, 480\n"
    "xvld $xr16, $t1, 512\n"
    "xvld $xr17, $t1, 544\n"
    "xvld $xr18, $t1, 576\n"
    "xvld $xr19, $t1, 608\n"
    "xvld $xr20, $t1, 640\n"
    "xvld $xr21, $t1, 672\n"
    "xvld $xr22, $t1, 704\n"
    "xvld $xr23, $t1, 736\n"
    "xvld $xr24, $t1, 768\n"
    "xvld $xr25, $t1, 800\n"
    "xvld $xr26, $t1, 832\n"
    "xvld $xr27, $t1, 864\n"
    "xvld $xr28, $t1, 896\n"
    "xvld $xr29, $t1, 928\n"
    "xvld $xr30, $t1, 960\n"
    "xvld $xr31, $t1, 992\n"
    "ld.w $t2, $t1, 1024\n"
    "movgr2fcsr $fcsr0, $t2\n"
    "ld.d $t2, $t1, 1032\n"
    ".word 0x005cfc2e\n"
    "ld.d $t2, $t1, 1040\n"
    "movgr2cf $fcc0, $t2\n"
    "ld.d $t2, $t1, 1048\n"
    "movgr2cf $fcc1, $t2\n"
    "ld.d $t2, $t1, 1056\n"
    "movgr2cf $fcc2, $t2\n"
    "ld.d $t2, $t1, 1064\n"
    "movgr2cf $fcc3, $t2\n"
    "ld.d $t2, $t1, 1072\n"
    "movgr2cf $fcc4, $t2\n"
    "ld.d $t2, $t1, 1080\n"
    "movgr2cf $fcc5, $t2\n"
    "ld.d $t2, $t1, 1088\n"
    "movgr2cf $fcc6, $t2\n"
    "ld.d $t2, $t1, 1096\n"
    "movgr2cf $fcc7, $t2\n"
    "ld.d $t1, $t0, 256\n"
    "csrwr $t1, 0x6\n"
    "ld.d $t1, $t0, 280\n"
    "csrwr $t1, 0x1\n"
    "ld.d $t1, $t0, 312\n"
    "csrwr $t1, 0x18\n"
    "ld.d $r1, $t0, 8\n"
    "ld.d $r2, $t0, 16\n"
    "ld.d $r3, $t0, 24\n"
    "ld.d $r4, $t0, 32\n"
    "ld.d $r5, $t0, 40\n"
    "ld.d $r6, $t0, 48\n"
    "ld.d $r7, $t0, 56\n"
    "ld.d $r8, $t0, 64\n"
    "ld.d $r9, $t0, 72\n"
    "ld.d $r10, $t0, 80\n"
    "ld.d $r11, $t0, 88\n"
    "ld.d $r14, $t0, 112\n"
    "ld.d $r15, $t0, 120\n"
    "ld.d $r16, $t0, 128\n"
    "ld.d $r17, $t0, 136\n"
    "ld.d $r18, $t0, 144\n"
    "ld.d $r19, $t0, 152\n"
    "ld.d $r20, $t0, 160\n"
    "ld.d $r21, $t0, 168\n"
    "ld.d $r22, $t0, 176\n"
    "ld.d $r23, $t0, 184\n"
    "ld.d $r24, $t0, 192\n"
    "ld.d $r25, $t0, 200\n"
    "ld.d $r26, $t0, 208\n"
    "ld.d $r27, $t0, 216\n"
    "ld.d $r28, $t0, 224\n"
    "ld.d $r29, $t0, 232\n"
    "ld.d $r30, $t0, 240\n"
    "ld.d $r31, $t0, 248\n"
    "ld.d $r13, $t0, 104\n"
    "ld.d $r12, $t0, 96\n"
    "ertn\n"
    ".size hamt_fault_restore_frame, .-hamt_fault_restore_frame\n"
);
