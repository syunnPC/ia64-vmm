/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global test_ia32_fp_switch
.type test_ia32_fp_switch,@function
test_ia32_fp_switch:
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8,%rsp
    mov %rdi,%r12
    mov %rsi,%r13
    mov %rdx,%r14
    mov %rcx,%r15
    xor %ecx,%ecx
    xgetbv
    xsave64 (%r14)
    call amd64_ia32_fp_enter
    mov %r12,%rdi
    mov %r13,%rsi
    call amd64_ia32_fp_leave
    xor %ecx,%ecx
    xgetbv
    xsave64 (%r15)
    add $8,%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    ret
.size test_ia32_fp_switch,.-test_ia32_fp_switch
.section .note.GNU-stack,"",@progbits
