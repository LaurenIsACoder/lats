/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Inline serial handshakes add no CALL to the 32-RET verification interval. */
.text
.macro SAMPLE name, length
    mov $1, %eax
    mov $1, %edi
    lea \name(%rip), %rsi
    mov $\length, %edx
    syscall
    cmp $\length, %rax
    jne .Lfail
    sub $16, %rsp
    xor %eax, %eax
    xor %edi, %edi
    mov %rsp, %rsi
    mov $2, %edx
    syscall
    add $16, %rsp
    cmp $2, %rax
    jne .Lfail
.endm

.globl ras_syscall_probe, ras_probe_begin, ras_probe_end
.type ras_syscall_probe, @function
ras_probe_begin:
ras_syscall_probe:
    sub $8, %rsp
    call .Llevel0
    cmpb $0, ras_probe_enabled(%rip)
    je .Louter_done
    /* Preserve the checksum without calling another function. */
    push %rax
    SAMPLE .Louter, 10
    pop %rax
.Louter_done:
    add $8, %rsp
    ret

.macro LEVEL number, next
.Llevel\number:
    sub $8, %rsp
    call .Llevel\next
    add $(\number+1), %rax
    add $8, %rsp
    ret
.endm
.altmacro
.set level, 0
.rept 31
    LEVEL %level, %(level+1)
    .set level, level+1
.endr
.noaltmacro
.Llevel31:
    cmpb $0, ras_probe_enabled(%rip)
    je .Lgetpid
    SAMPLE .Lpre, 8
.Lgetpid:
    mov $39, %eax
    syscall
    test %rax, %rax
    jle .Lfail
    cmpb $0, ras_probe_enabled(%rip)
    je .Lleaf_done
    SAMPLE .Lpost, 9
.Lleaf_done:
    mov $74, %eax /* 42 + this level's 32; other levels add 1..31 */
    ret
.Lfail:
    mov $60, %eax
    mov $99, %edi
    syscall
    ud2
ras_probe_end:
.size ras_syscall_probe, .-ras_syscall_probe
.section .rodata
.Lpre: .ascii "RAS_PRE\n"
.Lpost: .ascii "RAS_POST\n"
.Louter: .ascii "RAS_OUTER\n"
.section .note.GNU-stack,"",@progbits
