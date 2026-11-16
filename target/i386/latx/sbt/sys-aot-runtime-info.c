/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "host/cpuinfo.h"
#include "cpu.h"
#include "latx-ras-config.h"
#include "sys-aot-format.h"
#include "sys-aot-offline.h"
#include "latx-options.h"

static int aot_write_runtime_info_stream(FILE *out, const char *digest)
{
    fprintf(out, "{\n  \"binary_sha256\": \"%s\",\n", digest);
    fprintf(out, "  \"aot_version\": \"%s\",\n", AOT_VERSION);
#ifdef CONFIG_LATX_HACK_MEM
    fprintf(out, "  \"native_async_exit\": %s,\n",
            (cpuinfo & CPUINFO_LASX) ? "true" : "false");
    fprintf(out, "  \"native_direct_call_link\": %s,\n",
            (cpuinfo & CPUINFO_LASX) ? "true" : "false");
#endif
    fprintf(out, "  \"native_syscall\": true,\n");
    fprintf(out, "  \"native_cpu_query\": true,\n");
    fprintf(out, "  \"native_system\": true,\n");
    fprintf(out, "  \"native_atomic_return\": true,\n");
#ifdef CONFIG_LATX_HACK_MEM
    fprintf(out, "  \"native_hamt_atomic\": true,\n");
#endif
    fprintf(out, "  \"native_fp_environment\": true,\n");
    fprintf(out, "  \"native_x87\": %s,\n", option_soft_fpu ? "true" : "false");
#define FIELD(name, member) \
    fprintf(out, "  \"" name "\": %zu,\n", offsetof(CPUX86State, member))
    FIELD("ras_stack_offset", ras_tp.stack);
    FIELD("tu_generation_array_offset", tu_generation_array);
    FIELD("njc_cache_offset", latx_njc_cache_ptr);
    FIELD("hflags_offset", hflags);
    FIELD("eip_offset", eip);
    FIELD("aot_native_outer_sp_offset", aot_native_outer_sp);
    FIELD("native_atomic_offset", aot_native_atomic);
    FIELD("fs_base_offset", segs[R_FS].base);
    FIELD("gs_base_offset", segs[R_GS].base);
    FIELD("ldt_selector_offset", ldt.selector);
    FIELD("tr_selector_offset", tr.selector);
    FIELD("cr0_offset", cr[0]);
    FIELD("gdt_base_offset", gdt.base);
    FIELD("gdt_limit_offset", gdt.limit);
    FIELD("idt_base_offset", idt.base);
    FIELD("idt_limit_offset", idt.limit);
    FIELD("eflags_offset", eflags);
    FIELD("df_offset", df);
    FIELD("tsc_base_offset", tsc_base_offset);
    FIELD("tsc_offset", tsc_offset);
#ifdef CONFIG_TSC_FREQ
    fprintf(out, "  \"tsc_multiplier\": %d,\n", option_opt_rdtsc_freq);
#else
    fprintf(out, "  \"tsc_multiplier\": 1,\n");
#endif
    FIELD("mxcsr_offset", mxcsr);
    FIELD("old_mxcsr_offset", old_mxcsr);
#ifdef CONFIG_LATX_OPT_NOFP
    FIELD("nofp_offset", nofp);
    FIELD("fpstt_offset", fpstt);
    FIELD("fpus_offset", fpus);
    FIELD("fpuc_offset", fpuc);
    fprintf(out, "  \"fp_exception_offset\": %zu,\n",
            offsetof(CPUX86State, fp_status) +
                offsetof(float_status, float_exception_flags));
    FIELD("fptags_offset", fptags);
    FIELD("fpregs_offset", fpregs);
    fprintf(out, "  \"fpreg_stride\": %zu,\n", sizeof(FPReg));
#endif
#undef FIELD
    fprintf(out, "  \"ras_size\": %u,\n", LATX_RAS_TP_SIZE);
#ifdef CONFIG_LATX_PROFILER
    fprintf(out, "  \"llvm_profile_counter_offset\": %zu,\n",
            offsetof(TranslationBlock, profile.llvm_exec_times));
#endif
    fprintf(out, "  \"user_flags\": %u,\n",
            HF_CPL_MASK | HF_PE_MASK | HF_MP_MASK | HF_LMA_MASK | HF_CS64_MASK |
            HF_CS32_MASK | HF_SS32_MASK | HF_OSFXSR_MASK | HF_HAMT_EN_MASK);
    fprintf(out, "  \"user_cflags\": %u,\n", CF_CLUSTER_MASK);
    fprintf(out, "  \"irq_offset\": %td\n}\n",
            (ptrdiff_t)offsetof(X86CPU, neg.icount_decr.u32) -
            (ptrdiff_t)offsetof(X86CPU, env));
    return (fflush(out) != 0 || ferror(out)) ? 1 : 0;
}

int aot_runtime_info(void)
{
    g_autofree gchar *path = g_file_read_link("/proc/self/exe", NULL);
    g_autofree gchar *data = NULL;
    g_autofree gchar *digest = NULL;
    gsize bytes;

    if (!path || !g_file_get_contents(path, &data, &bytes, NULL)) {
        fprintf(stderr, "Cannot read the running executable\n");
        return 1;
    }
    digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                        (const guchar *)data, bytes);
    return aot_write_runtime_info_stream(stdout, digest);
}

bool aot_write_runtime_info_file(const char *output_path, bool skip_binary_hash)
{
    g_autofree gchar *path = g_file_read_link("/proc/self/exe", NULL);
    g_autofree gchar *data = NULL;
    g_autofree gchar *digest = NULL;

    if (skip_binary_hash) {
        struct stat st;
        uint8_t header[4096];
        ssize_t n = 0;
        int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
        if (fd >= 0 && fstat(fd, &st) == 0) {
            n = read(fd, header, sizeof(header));
            if (n < 0) {
                n = 0;
            }
            GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
            g_checksum_update(cs, (const guchar *)&st.st_size, sizeof(st.st_size));
            g_checksum_update(cs, (const guchar *)&st.st_mtim, sizeof(st.st_mtim));
            if (n > 0) {
                g_checksum_update(cs, header, (gssize)n);
            }
            digest = g_strdup(g_checksum_get_string(cs));
            g_checksum_free(cs);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (!digest) {
            return false;
        }
    } else {
        gsize bytes;
        if (!path || !g_file_get_contents(path, &data, &bytes, NULL)) {
            return false;
        }
        digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                             (const guchar *)data, bytes);
    }
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        return false;
    }
    int rc = aot_write_runtime_info_stream(out, digest);
    if (fclose(out) != 0) {
        return false;
    }
    return rc == 0;
}
