/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global amd64_load_gdt
.type amd64_load_gdt,@function
amd64_load_gdt:
    lgdt (%rdi)
    pushq $8
    lea 1f(%rip), %rax
    push %rax
    lretq
1:  mov $16, %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    /* Keep the per-CPU GS base valid even if an NMI lands here. */
    mov $24, %eax
    ltr %ax
    ret
.size amd64_load_gdt, .-amd64_load_gdt
.type exception_0,@function
exception_0:
    pushq $0
    pushq $0
    jmp exception_common
.type exception_1,@function
exception_1:
    pushq $0
    pushq $1
    jmp exception_common
.type exception_2,@function
exception_2:
    pushq $0
    pushq $2
    jmp exception_common
.type exception_3,@function
exception_3:
    pushq $0
    pushq $3
    jmp exception_common
.type exception_4,@function
exception_4:
    pushq $0
    pushq $4
    jmp exception_common
.type exception_5,@function
exception_5:
    pushq $0
    pushq $5
    jmp exception_common
.type exception_6,@function
exception_6:
    pushq $0
    pushq $6
    jmp exception_common
.type exception_7,@function
exception_7:
    pushq $0
    pushq $7
    jmp exception_common
.type exception_8,@function
exception_8:
    pushq $8
    jmp exception_common
.type exception_9,@function
exception_9:
    pushq $0
    pushq $9
    jmp exception_common
.type exception_10,@function
exception_10:
    pushq $10
    jmp exception_common
.type exception_11,@function
exception_11:
    pushq $11
    jmp exception_common
.type exception_12,@function
exception_12:
    pushq $12
    jmp exception_common
.type exception_13,@function
exception_13:
    pushq $13
    jmp exception_common
.type exception_14,@function
exception_14:
    pushq $14
    jmp exception_common
.type exception_15,@function
exception_15:
    pushq $0
    pushq $15
    jmp exception_common
.type exception_16,@function
exception_16:
    pushq $0
    pushq $16
    jmp exception_common
.type exception_17,@function
exception_17:
    pushq $17
    jmp exception_common
.type exception_18,@function
exception_18:
    pushq $0
    pushq $18
    jmp exception_common
.type exception_19,@function
exception_19:
    pushq $0
    pushq $19
    jmp exception_common
.type exception_20,@function
exception_20:
    pushq $0
    pushq $20
    jmp exception_common
.type exception_21,@function
exception_21:
    pushq $21
    jmp exception_common
.type exception_22,@function
exception_22:
    pushq $0
    pushq $22
    jmp exception_common
.type exception_23,@function
exception_23:
    pushq $0
    pushq $23
    jmp exception_common
.type exception_24,@function
exception_24:
    pushq $0
    pushq $24
    jmp exception_common
.type exception_25,@function
exception_25:
    pushq $0
    pushq $25
    jmp exception_common
.type exception_26,@function
exception_26:
    pushq $0
    pushq $26
    jmp exception_common
.type exception_27,@function
exception_27:
    pushq $0
    pushq $27
    jmp exception_common
.type exception_28,@function
exception_28:
    pushq $0
    pushq $28
    jmp exception_common
.type exception_29,@function
exception_29:
    pushq $29
    jmp exception_common
.type exception_30,@function
exception_30:
    pushq $30
    jmp exception_common
.type exception_31,@function
exception_31:
    pushq $0
    pushq $31
    jmp exception_common
exception_common:
    cld
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    mov %rsp, %rbx
    and $-16, %rsp
    call amd64_exception
    mov %rbx, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq
.section .rodata
.balign 8
.global amd64_exception_stubs
amd64_exception_stubs:
    .quad exception_0
    .quad exception_1
    .quad exception_2
    .quad exception_3
    .quad exception_4
    .quad exception_5
    .quad exception_6
    .quad exception_7
    .quad exception_8
    .quad exception_9
    .quad exception_10
    .quad exception_11
    .quad exception_12
    .quad exception_13
    .quad exception_14
    .quad exception_15
    .quad exception_16
    .quad exception_17
    .quad exception_18
    .quad exception_19
    .quad exception_20
    .quad exception_21
    .quad exception_22
    .quad exception_23
    .quad exception_24
    .quad exception_25
    .quad exception_26
    .quad exception_27
    .quad exception_28
    .quad exception_29
    .quad exception_30
    .quad exception_31
.section .note.GNU-stack,"",@progbits
