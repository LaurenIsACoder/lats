/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include "kernels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
static volatile uint64_t sink;
static int counter_handshake;
static void acknowledgement(const char *expected) {
    char line[32];
    if(!fgets(line,sizeof(line),stdin) || strcmp(line,expected))abort();
}
static uint64_t ns(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static int run(const char *name,uint64_t n) {
    const struct kernel *k=NULL;
    for(size_t i=0;i<kernel_count;i++)if(!strcmp(name,kernels[i].name))k=&kernels[i];
    if(!k || !n)return 2;
    sink=k->fn(n<100000?n:100000,42);
    printf("BENCH_BEGIN %s %llu\n",name,(unsigned long long)n);fflush(stdout);
    if(counter_handshake)acknowledgement("GO\n");
    uint64_t t=ns(),value=k->fn(n,42),elapsed=ns()-t;sink=value;
    if(counter_handshake) {
        printf("BENCH_STOP %s\n",name);fflush(stdout);
        acknowledgement("NEXT\n");
    }
    printf("BENCH_END {\"case\":\"%s\",\"iterations\":%llu,\"ns\":%llu,\"checksum\":\"%016llx\"}\n",name,(unsigned long long)n,(unsigned long long)elapsed,(unsigned long long)value);fflush(stdout);
    return 0;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(getpid()==1) {mount("proc","/proc","proc",0,NULL);mount("sysfs","/sys","sysfs",0,NULL);}
    prepare_data();
    if(getpid()==1 || (argc==2&&!strcmp(argv[1],"--server"))) {
        const char *handshake=getenv("BENCH_COUNTER_HANDSHAKE");
        counter_handshake=handshake && !strcmp(handshake,"1");
        puts(counter_handshake?"BENCH_READY GO/STOP/NEXT":"BENCH_READY");char line[128],name[64];unsigned long long n;
        while(fgets(line,sizeof(line),stdin)) {if(sscanf(line,"%63s %llu",name,&n)==2) {if(run(name,n))puts("BENCH_ERROR");}}
        return 0;
    }
    if(argc!=3) {fprintf(stderr,"usage: %s CASE ITERATIONS | --server\n",argv[0]);return 2;}
    char *end;unsigned long long n=strtoull(argv[2],&end,10);if(*end||argv[2][0]=='-')return 2;
    return run(argv[1],n);
}
