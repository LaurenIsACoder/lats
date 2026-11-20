/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 16-bit INVPCID in real, protected and virtual-8086 modes. */
.text
.code16
.Lrom:
#define OFF(x) (x-.Lrom)
#define RAM(x) (0x60000+OFF(x))
    cli
    cld
    movw %cs,%ax
    movw %ax,%ds
    movw $0x6000,%ax
    movw %ax,%es
    xorw %si,%si
    xorw %di,%di
    movw $0x8000,%cx
    rep movsw
    ljmp $0x6000,$OFF(.Lreal)
.Lreal:
    xorw %ax,%ax
    movw %ax,%ds
    movw %ax,%ss
    movw $0x7000,%sp
    movw $OFF(.Lreal_ud),0x18
    movw %cs,0x1a
    xorl %eax,%eax
    cpuid
    xorl %edi,%edi
    cmpl $0x68747541,%ebx       /* AuthenticAMD */
    sete %al
    movzbw %al,%di
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    btl $10,%ebx
    jnc .Lbad16
    movl $2,%eax
.Lreal_insn:
    invpcid (0x2000),%eax
    testw %di,%di
    jnz .Lbad16
    jmp .Lenter_pm
.Lreal_ud:
    popw %bx
    cmpw $OFF(.Lreal_insn),%bx
    jne .Lbad16
    testw %di,%di
    jz .Lbad16
    addw $4,%sp
.Lenter_pm:
    lgdtl %cs:OFF(.Lgdtr)
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmp $8,$OFF(.Lpm16)
.Lbad16:
    movb $'F',%al
    outb %al,$0xe9
    outl %eax,$0xf4
    hlt
.Lpm16:
    movw $16,%ax
    movw %ax,%ds
    movw %ax,%ss
    movl $0x7000,%esp
    lidtl %cs:OFF(.Lidtr)
    movl $2,%eax
    invpcid (0x2000),%eax
    /* Addr32 selects a different ModRM addressing form. */
    movl $0x2000,%ebx
    invpcid (%ebx),%eax
    ljmpl $24,$RAM(.Lenter_vm86)
.code32
.Lenter_vm86:
    movl $0x17000,0x18004
    movw $16,0x18008
    movw $32,%ax
    ltr %ax
    pushl $0                    /* GS, FS, DS, ES */
    pushl $0
    pushl $0
    pushl $0
    pushl $0                    /* SS:ESP */
    pushl $0x9000
    pushl $0x20002              /* EFLAGS.VM */
    pushl $0x6000
    pushl $OFF(.Lvm86)
    iretl
.code16
.Lvm86:
    movl $2,%eax
.Lvm86_insn:
    invpcid (0x2000),%eax
    ud2
.code32
.Lgp:
    popl %eax
    testl %eax,%eax
    jnz .Lbad
    testl %edi,%edi
    jnz .Lbad
    jmp .Lframe
.Lud:
    testl %edi,%edi
    jz .Lbad
.Lframe:
    cmpl $OFF(.Lvm86_insn),(%esp)
    jne .Lbad
    testl $0x20000,8(%esp)
    jz .Lbad
    movl $'S',%eax
    jmp .Lexit
.Lbad:
    movl $'F',%eax
.Lexit:
    outb %al,$0xe9
    outl %eax,$0xf4
    hlt
.balign 8
.Lgdt:
    .quad 0,0x00009b060000ffff,0x00cf93000000ffff,0x00cf9b000000ffff
    .quad 0x0000890180000067     /* 32-bit TSS at 0x18000 */
.Lgdtr:
    .word 39
    .long RAM(.Lgdt)
.macro gate label
    .word RAM(\label) & 0xffff,24,0x8e00,RAM(\label) >> 16
.endm
.Lidt:
    .space 6*8
    gate .Lud
    .space 6*8
    gate .Lgp
.Lidtr:
    .word 14*8-1
    .long RAM(.Lidt)
.code16
.org 0xfff0
    ljmp $0xf000,$0
.org 0x10000
