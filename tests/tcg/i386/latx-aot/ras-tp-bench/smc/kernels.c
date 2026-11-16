/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include "kernels.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stddef.h>
struct control { volatile unsigned mode,ready,release,warmed,go,done; volatile uintptr_t target; };
static uint64_t fold(uint64_t v,unsigned result) {return (v*6364136223846793005ULL)^result;}
#if defined(__x86_64__)
static void bind_cpu(unsigned which) {
    cpu_set_t set,one;int cpus[CPU_SETSIZE],n=0;
    if(sched_getaffinity(0,sizeof(set),&set))abort();
    for(int c=0;c<CPU_SETSIZE;++c)if(CPU_ISSET(c,&set))cpus[n++]=c;
    if(!n || (getpid()==1 && n<2))abort();
    CPU_ZERO(&one);CPU_SET(cpus[which%n],&one);
    if(sched_setaffinity(0,sizeof(one),&one))abort();
}
static uint64_t run_smc(uint64_t n,uint64_t seed,unsigned workers) {
    (void)workers;
    const char *executor_setting=getenv("SMC_EXECUTOR_CPU");
    unsigned executor=executor_setting ? (unsigned)strtoul(executor_setting,NULL,10) : 0;
    unsigned writer=executor ? 0 : 1;
    cpu_set_t original_affinity;
    if(sched_getaffinity(0,sizeof(original_affinity),&original_affinity))abort();
    struct control *state=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    unsigned char *code=mmap(NULL,12288,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(state==MAP_FAILED || code==MAP_FAILED)abort();
    /* Also exercise CALL and continuation on one protected physical page. */
    unsigned continuation=getenv("SMC_SAMEPAGE") ? 256 : 4096;
    unsigned entry=continuation-5;
    code[entry]=0xe8;int32_t displacement=8192-continuation;
    memcpy(code+entry+1,&displacement,4);
    code[continuation]=0xb8;code[continuation+5]=0xc3;
    unsigned char callee[]={0x48,0xba,0,0,0,0,0,0,0,0,0x83,0x7a,0,0,
        0x74,0x0d,0xc7,0x42,4,1,0,0,0,0x83,0x7a,8,0,0x74,0xfa,0xc3};
    uintptr_t address=(uintptr_t)state;memcpy(callee+2,&address,8);memcpy(code+8192,callee,sizeof(callee));
    state->target=(uintptr_t)(code+8192);
    if(getenv("SMC_INDIRECT")) {
        /* Load an alternating indirect target before the same continuation. */
        entry=continuation-15;code[entry]=0x48;code[entry+1]=0xb8;
        uintptr_t slot=(uintptr_t)&state->target;memcpy(code+entry+2,&slot,8);
        const unsigned char load_call[]={0x48,0x8b,0x00,0xff,0xd0};
        memcpy(code+entry+10,load_call,sizeof(load_call));
        memcpy(code+8256,callee,sizeof(callee));
    }
    uint32_t (*function)(void)=(void *)(code+entry);
    int pipefd[2];if(pipe(pipefd))abort();pid_t child=fork();if(child<0)abort();
    if(!child) {
        close(pipefd[0]);bind_cpu(executor);
        for(unsigned i=0;i<1000;++i)if(function()!=0)abort();
        state->warmed=1;uint64_t value=seed;
        for(unsigned i=0;i<n;++i) {
            while(state->go!=i+1){}
            state->mode=0;
            /* Restore the CALL link before each forced invalidation. */
            for(unsigned j=0;j<32;++j)if(function()!=i)abort();
            state->mode=1;
            unsigned result=function();if(result!=i+1)abort();
            value=fold(value,result);state->done=i+1;
        }
        printf("SMC executor cpu=%d rounds=%llu checksum=%016llx\n",sched_getcpu(),(unsigned long long)n,(unsigned long long)value);
        if(write(pipefd[1],&value,sizeof(value))!=sizeof(value))abort();_exit(0);
    }
    close(pipefd[1]);bind_cpu(writer);while(!state->warmed){}
    for(unsigned i=0;i<n;++i) {
        state->ready=0;state->release=0;
        state->target=(uintptr_t)(code+8192+((i&1)?64:0));
        state->go=i+1;
        while(!state->ready){}
        unsigned next=i+1;memcpy(code+continuation+1,&next,4);
        __asm__ __volatile__("" ::: "memory");state->release=1;
        while(state->done!=i+1){}
    }
    uint64_t value;int status;
    if(read(pipefd[0],&value,sizeof(value))!=sizeof(value))abort();close(pipefd[0]);
    if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))abort();
    printf("SMC writer cpu=%d rounds=%llu\n",sched_getcpu(),(unsigned long long)n);
    if(sched_setaffinity(0,sizeof(original_affinity),&original_affinity))abort();
    munmap(code,12288);munmap(state,4096);return value;
}
#else
static uint64_t run_smc(uint64_t n,uint64_t seed,unsigned workers) {
    (void)workers;for(unsigned i=0;i<n;++i)seed=fold(seed,i+1);return seed;
}
#endif
static uint64_t smc_return(uint64_t n,uint64_t seed) {return run_smc(n,seed,2);}
const struct kernel kernels[]={{"smc_return",smc_return}};
const size_t kernel_count=1;
void prepare_data(void) {}
