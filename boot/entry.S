/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global _start
.type _start,@function
_start:
    cli
    cld
    lea boot_stack_top(%rip), %rsp
    xor %ebp, %ebp
    call boot_main
1:  cli
    hlt
    jmp 1b
.size _start, .-_start

/* Leave the Limine stack and CR3 together. No return address or MP pointer
 * survives on the bootloader's stack after this transition. */
.global amd64_enter_cpu
.type amd64_enter_cpu,@function
amd64_enter_cpu:
    cli
    cld
    mov %rdi, %cr3
    mov %rsi, %rsp
    xor %ebp, %ebp
    mov %rcx, %rdi
    call *%rdx
2:  cli
    hlt
    jmp 2b
.size amd64_enter_cpu, .-amd64_enter_cpu
.section .bss
.balign 4096
.global boot_stack, boot_stack_top
boot_stack:
    .skip 65536
boot_stack_top:
.section .note.GNU-stack,"",@progbits
