/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include "kernels.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <errno.h>
#if defined(__x86_64__)
static uint64_t mm_pattern[8];
static struct { uint64_t lo, hi; } vec_pattern[16];
static void init_patterns(void) {
    for(unsigned i=0;i<8;++i) mm_pattern[i]=0x0123456789abcdefULL^(0x101010101010101ULL*i);
    for(unsigned i=0;i<16;++i) {
        vec_pattern[i].lo=0xfedcba9876543210ULL^(0x102030405060708ULL*i);
        vec_pattern[i].hi=0x2468ace013579bdfULL^(0x807060504030201ULL*i);
    }
}
static uint64_t observed[40];
__asm__(".text\n.globl fp_leaf\n.type fp_leaf,@function\nfp_leaf:\nret\n.size fp_leaf,.-fp_leaf\n");
__attribute__((noinline)) static void check_mapped(void) {
    __asm__ __volatile__(
        "movq 0(%[mm]), %%mm0\n\t"
        "movq 8(%[mm]), %%mm1\n\t"
        "movq 16(%[mm]), %%mm2\n\t"
        "movq 24(%[mm]), %%mm3\n\t"
        "movq 32(%[mm]), %%mm4\n\t"
        "movq 40(%[mm]), %%mm5\n\t"
        "movq 48(%[mm]), %%mm6\n\t"
        "movq 56(%[mm]), %%mm7\n\t"
        "movdqu 0(%[vec]), %%xmm0\n\t"
        "movdqu 16(%[vec]), %%xmm1\n\t"
        "movdqu 32(%[vec]), %%xmm2\n\t"
        "movdqu 48(%[vec]), %%xmm3\n\t"
        "movdqu 64(%[vec]), %%xmm4\n\t"
        "movdqu 80(%[vec]), %%xmm5\n\t"
        "movdqu 96(%[vec]), %%xmm6\n\t"
        "movdqu 112(%[vec]), %%xmm7\n\t"
        "movdqu 128(%[vec]), %%xmm8\n\t"
        "movdqu 144(%[vec]), %%xmm9\n\t"
        "movdqu 160(%[vec]), %%xmm10\n\t"
        "movdqu 176(%[vec]), %%xmm11\n\t"
        "movdqu 192(%[vec]), %%xmm12\n\t"
        "movdqu 208(%[vec]), %%xmm13\n\t"
        "movdqu 224(%[vec]), %%xmm14\n\t"
        "movdqu 240(%[vec]), %%xmm15\n\t"
        "call fp_leaf\n\t"
        ".globl fp_after_call\n\t"
        "fp_after_call:\n\t"
        "movq %%mm0, 0(%[out])\n\t"
        "movq %%mm1, 8(%[out])\n\t"
        "movq %%mm2, 16(%[out])\n\t"
        "movq %%mm3, 24(%[out])\n\t"
        "movq %%mm4, 32(%[out])\n\t"
        "movq %%mm5, 40(%[out])\n\t"
        "movq %%mm6, 48(%[out])\n\t"
        "movq %%mm7, 56(%[out])\n\t"
        "movdqu %%xmm0, 64(%[out])\n\t"
        "movdqu %%xmm1, 80(%[out])\n\t"
        "movdqu %%xmm2, 96(%[out])\n\t"
        "movdqu %%xmm3, 112(%[out])\n\t"
        "movdqu %%xmm4, 128(%[out])\n\t"
        "movdqu %%xmm5, 144(%[out])\n\t"
        "movdqu %%xmm6, 160(%[out])\n\t"
        "movdqu %%xmm7, 176(%[out])\n\t"
        "movdqu %%xmm8, 192(%[out])\n\t"
        "movdqu %%xmm9, 208(%[out])\n\t"
        "movdqu %%xmm10, 224(%[out])\n\t"
        "movdqu %%xmm11, 240(%[out])\n\t"
        "movdqu %%xmm12, 256(%[out])\n\t"
        "movdqu %%xmm13, 272(%[out])\n\t"
        "movdqu %%xmm14, 288(%[out])\n\t"
        "movdqu %%xmm15, 304(%[out])\n\t"
        "emms\n\t"
        :
        : [mm] "r"(mm_pattern), [vec] "r"(vec_pattern), [out] "r"(observed)
        : "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    for (unsigned i=0; i<8; ++i) if(observed[i]!=mm_pattern[i]) {fprintf(stderr,"MMX %u corrupted\n",i);abort();}
    for (unsigned i=0; i<16; ++i) if(observed[8+2*i]!=vec_pattern[i].lo || observed[9+2*i]!=vec_pattern[i].hi) {fprintf(stderr,"XMM %u corrupted\n",i);abort();}
}
static uint64_t x87_observed[3];
static const double x87_pattern[3]={1.0,2.0,4.0};
static const uint64_t x87_expected[3]={0x4010000000000000ULL,0x4000000000000000ULL,0x3ff0000000000000ULL};
__attribute__((noinline)) static void check_x87(void) {
    __asm__ __volatile__(
        "fninit\n\tfldl 0(%[in])\n\tfldl 8(%[in])\n\tfldl 16(%[in])\n\t"
        "call fp_leaf\n\t.globl x87_after_call\nx87_after_call:\n\t"
        "fstpl 0(%[out])\n\tfstpl 8(%[out])\n\tfstpl 16(%[out])\n\tfninit"
        : : [out] "r"(x87_observed), [in] "r"(x87_pattern) : "st", "st(1)", "st(2)", "memory", "cc");
    for(unsigned i=0;i<3;++i) if(x87_observed[i]!=x87_expected[i]) {fprintf(stderr,"x87 %u corrupted\n",i);abort();}
}
#else
static void check_mapped(void) {}
static void check_x87(void) {}
#endif
static uint64_t mapped_fp(uint64_t n,uint64_t seed) {
    uint64_t value=seed;
    for (uint64_t i=0;i<n;++i) {check_mapped();value=(value*6364136223846793005ULL)^i;}
    return value;
}
static uint64_t x87_fp(uint64_t n,uint64_t seed) {
    uint64_t value=seed;
    for(uint64_t i=0;i<n;++i) {check_x87();value=(value*6364136223846793005ULL)^i;}
    return value;
}

__attribute__((noinline)) static uint64_t nested(unsigned depth,uint64_t value) {
    if (!depth) return value*6364136223846793005ULL;
    return nested(depth-1,value+depth)^depth;
}
struct worker_result { uint64_t value; unsigned index; int cpu; };
static uint64_t multi(uint64_t n,uint64_t seed,unsigned workers) {
    cpu_set_t available;
    if(sched_getaffinity(0,sizeof(available),&available))abort();
    int cpus[CPU_SETSIZE],nr=0;
    for(int c=0;c<CPU_SETSIZE;++c)if(CPU_ISSET(c,&available))cpus[nr++]=c;
    if(!nr || (getpid()==1 && nr<(int)workers))abort();
    int fds[4][2];pid_t pids[4];
    for(unsigned w=0;w<workers;++w) {
        if(pipe(fds[w]))abort();
        pids[w]=fork();if(pids[w]<0)abort();
        if(!pids[w]) {
            close(fds[w][0]);cpu_set_t one;CPU_ZERO(&one);CPU_SET(cpus[w%nr],&one);
            if(sched_setaffinity(0,sizeof(one),&one))abort();
#if defined(__x86_64__)
            for(unsigned reg=0;reg<8;++reg) mm_pattern[reg]^=w*0x1111111111111111ULL;
            for(unsigned reg=0;reg<16;++reg) {
                vec_pattern[reg].lo^=w*0x2222222222222222ULL;
                vec_pattern[reg].hi^=w*0x3333333333333333ULL;
            }
#endif
            uint64_t value=seed+w;
            for(uint64_t i=0;i<n;++i) {
                unsigned depths[]={1,17,511,512,513,600};
                value=nested(depths[i%6],value)^i;
                check_mapped();check_x87();
            }
            struct worker_result r={value,w,sched_getcpu()};
            if(r.cpu!=cpus[w%nr] || write(fds[w][1],&r,sizeof(r))!=sizeof(r))abort();
            _exit(0);
        }
        close(fds[w][1]);
    }
    uint64_t value=0;
    for(unsigned w=0;w<workers;++w) {
        struct worker_result r;int status;
        if(read(fds[w][0],&r,sizeof(r))!=sizeof(r))abort();
        close(fds[w][0]);
        if(waitpid(pids[w],&status,0)!=pids[w] || !WIFEXITED(status) || WEXITSTATUS(status))abort();
        if(r.index!=w || r.cpu!=cpus[w%nr])abort();
        printf("WORKER index=%u cpu=%d iterations=%llu checksum=%016llx\n",w,r.cpu,(unsigned long long)n,(unsigned long long)r.value);
        value^=r.value;
    }
    return value;
}
static uint64_t multi2(uint64_t n,uint64_t seed) {return multi(n,seed,2);}
static uint64_t multi4(uint64_t n,uint64_t seed) {return multi(n,seed,4);}
const struct kernel kernels[]={{"mapped_fp",mapped_fp},{"x87_fp",x87_fp},{"multi2",multi2},{"multi4",multi4}};
const size_t kernel_count=4;
void prepare_data(void) {
#if defined(__x86_64__)
    init_patterns();
#endif
}
