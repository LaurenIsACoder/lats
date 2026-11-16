/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "kernels.h"
#include <stdlib.h>
#define NOINLINE __attribute__((noinline))
static uint8_t bits[16384], predictable[16384];
static uint32_t chain_small[4096], chain_large[4194304];
extern uint64_t callee0(uint64_t), callee1(uint64_t), callee2(uint64_t), callee3(uint64_t);
static uint64_t (* volatile targets[4])(uint64_t) = {callee0,callee1,callee2,callee3};
static uint64_t rng(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void chain(uint32_t *a, uint32_t n) {
    uint32_t *order = malloc((size_t)n*sizeof(*order));
    if (!order) abort();
    for (uint32_t i=0;i<n;i++) order[i]=i;
    uint64_t s=UINT64_C(0x123456789abcdef);
    for(uint32_t i=n-1;i>0;i--) { uint32_t j=rng(&s)%(i+1), t=order[i];order[i]=order[j];order[j]=t; }
    for(uint32_t i=0;i<n;i++) a[order[i]]=order[(i+1)%n];
    free(order);
}
void prepare_data(void) {
    uint64_t s=UINT64_C(0x123456789abcdef);
    for(unsigned i=0;i<16384;i++) { bits[i]=rng(&s)&1; predictable[i]=i%32!=0; }
    chain(chain_small,4096); chain(chain_large,4194304);
}
NOINLINE static uint64_t loop_dep(uint64_t n,uint64_t s) {
    for(uint64_t i=0;i<n;i++) { s^=s<<7;s^=s>>9;s+=i; } return s;
}
NOINLINE static uint64_t loop_ilp(uint64_t n,uint64_t s) {
    uint64_t a=s,b=s+1,c=s+2,d=s+3;
    for(uint64_t i=0;i<n;i++) {a=(a<<1)^i;b=(b<<1)+i;c=(c>>1)^i;d=(d>>1)+i;}return a^b^c^d;
}
NOINLINE static uint64_t branch_pred(uint64_t n,uint64_t s) {
    for(uint64_t i=0;i<n;i++) {if(predictable[i&16383])s=(s<<1)+i;else s=(s>>1)^i;} return s;
}
NOINLINE static uint64_t branch_random(uint64_t n,uint64_t s) {
    for(uint64_t i=0;i<n;i++) {if(bits[i&16383])s=(s<<1)+i;else s=(s>>1)^i;} return s;
}
NOINLINE static uint64_t call_direct(uint64_t n,uint64_t s) { for(uint64_t i=0;i<n;i++)s=callee0(s);return s; }
NOINLINE static uint64_t call_indirect1(uint64_t n,uint64_t s) { for(uint64_t i=0;i<n;i++)s=targets[0](s);return s; }
NOINLINE static uint64_t call_indirect4(uint64_t n,uint64_t s) { for(uint64_t i=0;i<n;i++)s=targets[i&3](s);return s; }
#ifdef NATIVE_CHASE_OPT
extern uint64_t native_chase(uint64_t,uint64_t,const uint32_t *);
NOINLINE static uint64_t chase_l1(uint64_t n,uint64_t s) {return native_chase(n,s&4095,chain_small);}
NOINLINE static uint64_t chase_large(uint64_t n,uint64_t s) {return native_chase(n,s&4194303,chain_large);}
#else
NOINLINE static uint64_t chase_l1(uint64_t n,uint64_t s) { uint32_t p=s&4095;for(uint64_t i=0;i<n;i++)p=chain_small[p];return p; }
NOINLINE static uint64_t chase_large(uint64_t n,uint64_t s) { uint32_t p=s&4194303;for(uint64_t i=0;i<n;i++)p=chain_large[p];return p; }
#endif
const struct kernel kernels[]={
    {"loop_dep",loop_dep},{"loop_ilp",loop_ilp},{"branch_pred",branch_pred},{"branch_random",branch_random},
    {"call_direct",call_direct},{"call_indirect1",call_indirect1},{"call_indirect4",call_indirect4},
    {"chase_l1",chase_l1},{"chase_large",chase_large}
};
const size_t kernel_count=sizeof(kernels)/sizeof(kernels[0]);
