/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Included by pcid.S with HAMT_PTW_CONTRACT. R12 identifies the check on
 * failure. Identity-mapped code and page tables stay outside the aliases
 * whose permissions are revoked below.
 */
.Lptw_contract:
    movl $0x5a5a5a5a,CODE_A
    movq $0xe3,0x3008
    movl $ALIAS,%esi
    movq $CODE_A+3,PT_A
    invlpg (%rsi)
    movl $12,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $14,%r12d
    testq $32,PT_A
    jz .Lfail
    movl $16,%r12d
    testq $64,PT_A
    jnz .Lfail
    movl $0x5a5a5a5a,(%rsi)
    movl $19,%r12d
    testq $64,PT_A
    jz .Lfail
    movq $CODE_A+0x23,PT_A
    invlpg (%rsi)
    movl $23,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $25,%r12d
    testq $32,PT_A
    jz .Lfail
    movl $27,%r12d
    testq $64,PT_A
    jnz .Lfail
    movl $0x5a5a5a5a,(%rsi)
    movl $30,%r12d
    testq $64,PT_A
    jz .Lfail
    movq $CODE_A+0x61,PT_A
    invlpg (%rsi)
    movl $34,%r12d
    fault 14,3
    movl $0x5a5a5a5a,(%rsi)
    checkfault
    movq %cr2,%rax
    cmpq %rsi,%rax
    jne .Lfail
    movq $CODE_A+0x63,PT_A
    invlpg (%rsi)
    movl $0x309000,%esi
    movq $131,0x3008
    invlpg (%rsi)
    movl $45,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $47,%r12d
    testq $32,0x3008
    jz .Lfail
    movl $49,%r12d
    testq $64,0x3008
    jnz .Lfail
    movl $0x5a5a5a5a,(%rsi)
    movl $52,%r12d
    testq $64,0x3008
    jz .Lfail
    movq $163,0x3008
    invlpg (%rsi)
    movl $56,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $58,%r12d
    testq $32,0x3008
    jz .Lfail
    movl $60,%r12d
    testq $64,0x3008
    jnz .Lfail
    movl $0x5a5a5a5a,(%rsi)
    movl $63,%r12d
    testq $64,0x3008
    jz .Lfail
    movq $225,0x3008
    invlpg (%rsi)
    movl $67,%r12d
    fault 14,3
    movl $0x5a5a5a5a,(%rsi)
    checkfault
    movq %cr2,%rax
    cmpq %rsi,%rax
    jne .Lfail
    movq $227,0x3008
    invlpg (%rsi)
    movl $ALIAS,%esi
    andq $-33,PML4_A
    invlpg (%rsi)
    movl $78,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $80,%r12d
    testq $32,PML4_A
    jz .Lfail
    andq $-33,0x2000
    invlpg (%rsi)
    movl $84,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $86,%r12d
    testq $32,0x2000
    jz .Lfail
    andq $-33,0x3010
    invlpg (%rsi)
    movl $90,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    movl $92,%r12d
    testq $32,0x3010
    jz .Lfail

    andq $-3,0x3010
    invlpg (%rsi)
    movl $97,%r12d
    fault 14,3
    movl $0x5a5a5a5a,(%rsi)
    checkfault
    orq $2,0x3010
    invlpg (%rsi)
    /* Reserved L4 PS with A already set must still fault. */
    movq $0x20a3,0x1008
    movabsq $0x8000109000,%rsi
    invlpg (%rsi)
    /* Match the existing QEMU walker RSVD error encoding. */
    movl $107,%r12d
    fault 14,8
    movl (%rsi),%eax
    checkfault
    movq %cr2,%rax
    cmpq %rsi,%rax
    jne .Lfail
    /* A=1 1G leaves must not enter the incomplete 4K2M branch. */
    movq $0xe3,0x2008
    movl $0x40109000,%esi
    invlpg (%rsi)
    movl $117,%r12d
    cmpl $0x5a5a5a5a,(%rsi)
    jne .Lfail
    ret
