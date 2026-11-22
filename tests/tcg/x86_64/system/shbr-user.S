/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * The JIT TU emitter runs user code. boot.S has already identity-mapped
 * RAM with user-accessible 2 MiB pages. Make the first page (test text)
 * read-only and put the test stack in the next, writable page.
 */
.text
.globl __wrap_main
__wrap_main:
    mov %cr4, %rax
    or $0x200, %eax              /* CR4.OSFXSR */
    mov %rax, %cr4

    lgdt user_gdtr(%rip)
    mov %cr3, %rax
    mov (%rax), %rcx
    and $-4096, %rcx
    mov (%rcx), %rcx
    and $-4096, %rcx
    andq $-3, (%rcx)             /* Clear PDE.RW for the test text. */
    mov %rax, %cr3

    mov $0x400000, %edi
    mov (%rsp), %rax             /* Return to boot.S's test exit handler. */
    sub $8, %rdi
    mov %rax, (%rdi)
    pushq $0x23
    push %rdi
    pushq $0x3002                /* Allow debug console and exit port I/O. */
    pushq $0x1b
    lea __real_main(%rip), %rax
    push %rax
    iretq

.section .rodata
.balign 8
user_gdt:
    .quad 0
    .quad 0x00af9b000000ffff     /* Kernel code and data (boot.S selectors). */
    .quad 0x00cf93000000ffff
    .quad 0x00affb000000ffff     /* User code and data. */
    .quad 0x00cff3000000ffff
user_gdtr:
    .word . - user_gdt - 1
    .quad user_gdt

.section .note.GNU-stack,"",@progbits
