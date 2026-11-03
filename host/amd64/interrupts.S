/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
/* Save all integer registers, including generated-code temporaries. The
 * handlers and everything they call are general-registers-only code
 * (BM_GPR_ONLY_SOURCES, mk/check-gpr-only.py): x87, SSE and AVX state of
 * the interrupted code, MXCSR included, stays in the registers untouched. */
.altmacro
.macro INTERRUPT number
.type interrupt_\number,@function
interrupt_\number:
    pushq $\number
    jmp interrupt_common
.endm
.set vector, 32
.rept 224
    INTERRUPT %vector
    .set vector, vector+1
.endr
interrupt_common:
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
    mov 120(%rsp), %rdi
    mov 128(%rsp), %rsi
    mov %rsp, %rbx
    and $-16, %rsp
    call amd64_interrupt
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
    add $8, %rsp
    iretq
.section .rodata
.balign 8
.global amd64_interrupt_stubs
amd64_interrupt_stubs:
.macro POINTER number
    .quad interrupt_\number
.endm
.set vector, 32
.rept 224
    POINTER %vector
    .set vector, vector+1
.endr
.section .note.GNU-stack,"",@progbits
