/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native debug-register bracket around the shared VMX GPR/FP transport.
 * Arguments: GPRs, launched, FP pair, guest DR0..3/DR6. IF must be clear. */
.section .text
.global amd64_vmx_ia32_vmrun
.type amd64_vmx_ia32_vmrun,@function
amd64_vmx_ia32_vmrun:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $72, %rsp
    mov %rcx, %r12
    mov %rdi, %r13
    mov %esi, %r14d
    mov %rdx, %r15
    mov %dr0, %rax
    mov %rax, 0(%rsp)
    mov %dr1, %rax
    mov %rax, 8(%rsp)
    mov %dr2, %rax
    mov %rax, 16(%rsp)
    mov %dr3, %rax
    mov %rax, 24(%rsp)
    mov %dr6, %rax
    mov %rax, 32(%rsp)
    mov %dr7, %rax
    mov %rax, 40(%rsp)
    mov $0x400, %eax
    mov %rax, %dr7
    mov %cr2, %rax
    mov %rax, 48(%rsp)
    mov $0x1d9, %ecx
    rdmsr
    shl $32, %rdx
    or %rdx, %rax
    mov %rax, 56(%rsp)
    xor %eax, %eax
    xor %edx, %edx
    wrmsr
    mov 0(%r12), %rax
    mov %rax, %dr0
    mov 8(%r12), %rax
    mov %rax, %dr1
    mov 16(%r12), %rax
    mov %rax, %dr2
    mov 24(%r12), %rax
    mov %rax, %dr3
    mov 32(%r12), %rax
    mov %rax, %dr6
    mov %r13, %rdi
    mov %r14d, %esi
    mov %r15, %rdx
    call amd64_vmx_vmrun
    mov %eax, %r15d
    mov %dr0, %rax
    mov %rax, 0(%r12)
    mov %dr1, %rax
    mov %rax, 8(%r12)
    mov %dr2, %rax
    mov %rax, 16(%r12)
    mov %dr3, %rax
    mov %rax, 24(%r12)
    mov %dr6, %rax
    mov %rax, 32(%r12)
    mov 0(%rsp), %rax
    mov %rax, %dr0
    mov 8(%rsp), %rax
    mov %rax, %dr1
    mov 16(%rsp), %rax
    mov %rax, %dr2
    mov 24(%rsp), %rax
    mov %rax, %dr3
    mov 32(%rsp), %rax
    mov %rax, %dr6
    mov 48(%rsp), %rax
    mov %rax, %cr2
    mov 56(%rsp), %rax
    mov %rax, %rdx
    shr $32, %rdx
    mov $0x1d9, %ecx
    wrmsr
    mov 40(%rsp), %rax
    mov %rax, %dr7
    mov %r15d, %eax
    add $72, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size amd64_vmx_ia32_vmrun, .-amd64_vmx_ia32_vmrun
.section .note.GNU-stack,"",@progbits
