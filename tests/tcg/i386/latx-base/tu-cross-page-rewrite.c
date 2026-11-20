/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef TU_TEST_ASYNC
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
static uintptr_t code_begin, code_end;
static volatile sig_atomic_t signals_in_code, signals_bad_regs, signals_checked;
static void timer_handler(int signo, siginfo_t *info, void *context)
{
    (void)signo;
    (void)info;
    ucontext_t *uc=context;
    uintptr_t pc=uc->uc_mcontext.gregs[REG_RIP];
    if(pc>=code_begin && pc<code_end) {
        signals_in_code++;
        uintptr_t offset = pc - code_begin;
        if (offset == 65536 + 10 || offset == 65536 + 13 ||
            offset == 65536 + 15 || offset == 65536 + 17) {
            uint64_t a = uc->uc_mcontext.gregs[REG_RAX];
            uint64_t c = uc->uc_mcontext.gregs[REG_RCX];
            uint64_t sum = offset == 65536 + 13 ? 4098 : 4097;
            signals_checked++;
            if (a > 4097 || c > 4096 || a + c != sum) {
                signals_bad_regs++;
            }
        }
    }
}
#endif
#ifndef TU_TEST_ADDRESS
#define TU_TEST_ADDRESS 0
#endif
#ifndef TU_TEST_REPEATS
#define TU_TEST_REPEATS 1000
#endif
#ifdef TU_TEST_PRESSURE
static int code_pressure(void)
{
    const size_t count = 65536, stride = 512, adds = 120;
    size_t size = count * stride;
    unsigned char *code = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 14;
    for (size_t i = 0; i < count; i++) {
        unsigned char *f = code + i * stride;
        uint32_t value = i;
        f[0] = 0xb8; /* mov eax, unique immediate */
        memcpy(f + 1, &value, sizeof(value));
        for (size_t j = 0; j < adds; j++) {
            memcpy(f + 5 + j * 3, "\x83\xc0\x01", 3);
        }
        f[5 + adds * 3] = 0xc3;
    }
    if (mprotect(code, size, PROT_READ | PROT_EXEC)) return 15;
    puts("PRESSURE_BEGIN");
    fflush(stdout);
    for (size_t i = 0; i < count; i++) {
        int (*fn)(void) = (void *)(code + i * stride);
        if (fn() != (int)(i + adds)) return 16;
    }
    puts("PRESSURE_END");
    fflush(stdout);
    return munmap(code, size) ? 17 : 0;
}
#endif
int main(void) {
    size_t page=65536, size=page*2;
    void *hint=(void *)(uintptr_t)TU_TEST_ADDRESS;
    unsigned char *p=mmap(hint,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED)return 2;
    if(hint && p!=hint){munmap(p,size);return 9;}
    printf("TU_TEST_ENTRY=%p TARGET=%p\n",(void *)p,(void *)(p+page));
    /* Jcc exploration can introduce a new TU page; JMP only visits known pages. */
    unsigned char entry[]={0x85,0xff,0x0f,0x84,0,0,0,0,
                           0xb8,7,0,0,0,0xc3};
    memcpy(p,entry,sizeof(entry));
    int32_t disp=page-8;memcpy(p+4,&disp,4);
    unsigned char code[]={0xb8,1,0,0,0,0xb9,0x00,0x10,0,0,0x83,0xc0,1,0xff,0xc9,0x75,0xf9,0xc3};
#ifdef TU_TEST_RETURN_JMP
    unsigned char return_code[sizeof(code)+4];
    memcpy(return_code,code,sizeof(code));
    return_code[17]=0xe9;
    int32_t back_disp=32-(int32_t)(page+22);
    memcpy(return_code+18,&back_disp,4);
    p[32]=0xc3;
#define TARGET_CODE return_code
#else
#define TARGET_CODE code
#endif
    memcpy(p+page,TARGET_CODE,sizeof(TARGET_CODE));
    if(mprotect(p,size,PROT_READ|PROT_EXEC))return 3;
    int (*fn)(int)=(void*)p;
#ifdef TU_TEST_ASYNC
    code_begin=(uintptr_t)p;
    code_end=(uintptr_t)p+size;
    struct sigaction action={.sa_sigaction=timer_handler,.sa_flags=SA_SIGINFO};
    struct itimerval timer={.it_interval={.tv_usec=1000},
                            .it_value={.tv_usec=1000}};
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGALRM,&action,NULL) || setitimer(ITIMER_REAL,&timer,NULL))return 10;
    for(int i=0;i<100000;i++) {
        if(fn(0)!=4097 || fn(1)!=7)return 11;
    }
    struct itimerval stopped={0};
    if(setitimer(ITIMER_REAL,&stopped,NULL))return 12;
    printf("TU_ASYNC_IN_CODE=%d\n",(int)signals_in_code);
    printf("TU_ASYNC_CHECKED=%d BAD_REGS=%d\n",
           (int)signals_checked,(int)signals_bad_regs);
    if(!signals_in_code || !signals_checked || signals_bad_regs)return 13;
#endif
    for(int round=0;round<4;round++) {
        int expected=4097+round;
        for(int i=0;i<TU_TEST_REPEATS;i++) {
            if(fn(0)!=expected || fn(1)!=7){
                fprintf(stderr,"stale/conditional code round=%d\n",round);return 1;
            }
        }
        if(round==3)break;
        if(mprotect(p+page,page,PROT_READ|PROT_WRITE))return 4;
        p[page+1]=2+round;
        if(mprotect(p+page,page,PROT_READ|PROT_EXEC))return 5;
    }
    puts("cross-page rewrite passed");
#ifdef TU_TEST_PRESSURE
    int pressure_rc = code_pressure();
    if (pressure_rc) return pressure_rc;
    for (int i = 0; i < TU_TEST_REPEATS; i++) {
        if (fn(0) != 4100 || fn(1) != 7) return 18;
    }
    puts("post-pressure original entry passed");
#endif
    for(int round=0;round<4;round++) {
        if(munmap(p+page,page))return 6;
        void *replacement=mmap(p+page,page,PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
        if(replacement==MAP_FAILED)return 7;
        memcpy(p+page,TARGET_CODE,sizeof(TARGET_CODE));
        p[page+1]=10+round;
        if(mprotect(p+page,page,PROT_READ|PROT_EXEC))return 8;
        for(int i=0;i<TU_TEST_REPEATS;i++)if(fn(0)!=4106+round || fn(1)!=7){
            fprintf(stderr,"stale remapped code round=%d\n",round);return 1;
        }
    }
    puts("cross-page remap passed");return munmap(p,size)!=0;
}
