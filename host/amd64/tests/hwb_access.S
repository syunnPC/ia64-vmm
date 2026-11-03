/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global amd64_hwb_load, amd64_hwb_load_site
amd64_hwb_load:
amd64_hwb_load_site:
    mov (%rdi), %rax
    ret
.global amd64_hwb_store, amd64_hwb_store_site
amd64_hwb_store:
amd64_hwb_store_site:
    mov %rsi, (%rdi)
    ret
/* uint64_t amd64_hwb_simd_access(uintptr_t address): 0, or 1 when the load
 * faulted and the trap client sent it to the landing (no resolution). */
.global amd64_hwb_simd_access, amd64_hwb_simd_site, amd64_hwb_simd_landing
amd64_hwb_simd_access:
    xor %eax, %eax
amd64_hwb_simd_site:
    mov (%rdi), %rdx
    ret
amd64_hwb_simd_landing:
    mov $1, %eax
    ret
.global amd64_hwb_load_retry
amd64_hwb_load_retry:
    pushfq
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
    sub $8, %rsp
    call amd64_hwb_resolve
    add $8, %rsp
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
    popfq
    jmp amd64_hwb_load_site
.global amd64_hwb_store_retry
amd64_hwb_store_retry:
    pushfq
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
    sub $8, %rsp
    call amd64_hwb_resolve
    add $8, %rsp
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
    popfq
    jmp amd64_hwb_store_site
.section .note.GNU-stack,"",@progbits
