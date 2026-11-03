/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * void amd64_svm_vmrun(uint64_t vmcb_pa, uint64_t host_pa, struct svm_gprs *g,
 *                      const struct amd64_xstate_pair *x)
 *
 * One VMRUN of the real-mode container.
 * Caller: CLGI done and RFLAGS.IF clear. VMRUN saves and restores only
 * RAX, RSP, RIP, RFLAGS, ES/CS/SS/DS, GDTR/IDTR and the control registers
 * (APM Vol.2 §15.5); the other integer registers hold the guest's values
 * across it. FS, GS (the host's per-CPU base), TR (its IST stacks), LDTR,
 * KernelGsBase and the SYSCALL/SYSENTER MSRs move only with VMSAVE/VMLOAD
 * (§15.5.2): the host's are saved first and reloaded before returning.
 * Neither moves x87, SSE or AVX state, nor XCR0: the host's enabled state
 * is saved to x->host and the guest's loaded from x->guest before VMRUN,
 * and the reverse after it. GIF stays clear until the caller's STGI, so no
 * interrupt or NMI can run host code on the guest's GS base, TR or SIMD
 * state.
 */
#define G_RBX 0
#define G_RCX 8
#define G_RDX 16
#define G_RSI 24
#define G_RDI 32
#define G_RBP 40
#define G_R8 48
#define G_R9 56
#define G_R10 64
#define G_R11 72
#define G_R12 80
#define G_R13 88
#define G_R14 96
#define G_R15 104
.section .text
.global amd64_svm_vmrun
.type amd64_svm_vmrun,@function
amd64_svm_vmrun:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rcx
    push %rsi
    push %rdi
    push %rdx
    mov (%rcx), %rdi
    mov 8(%rcx), %rsi
    call amd64_xstate_switch
    mov (%rsp), %rdx
    mov 16(%rsp), %rax
    vmsave %rax
    mov 8(%rsp), %rax
    vmload %rax
    mov G_RBX(%rdx), %rbx
    mov G_RCX(%rdx), %rcx
    mov G_RSI(%rdx), %rsi
    mov G_RDI(%rdx), %rdi
    mov G_RBP(%rdx), %rbp
    mov G_R8(%rdx), %r8
    mov G_R9(%rdx), %r9
    mov G_R10(%rdx), %r10
    mov G_R11(%rdx), %r11
    mov G_R12(%rdx), %r12
    mov G_R13(%rdx), %r13
    mov G_R14(%rdx), %r14
    mov G_R15(%rdx), %r15
    mov G_RDX(%rdx), %rdx
    /* GIF=0: nothing is delivered here, but VMRUN records host IF=1, which
     * V_INTR_MASKING uses to let host interrupts exit the guest. */
    sti
    vmrun %rax
    cli
    xchg %rdx, (%rsp)
    mov %rbx, G_RBX(%rdx)
    mov %rcx, G_RCX(%rdx)
    mov %rsi, G_RSI(%rdx)
    mov %rdi, G_RDI(%rdx)
    mov %rbp, G_RBP(%rdx)
    mov %r8, G_R8(%rdx)
    mov %r9, G_R9(%rdx)
    mov %r10, G_R10(%rdx)
    mov %r11, G_R11(%rdx)
    mov %r12, G_R12(%rdx)
    mov %r13, G_R13(%rdx)
    mov %r14, G_R14(%rdx)
    mov %r15, G_R15(%rdx)
    pop %rcx
    mov %rcx, G_RDX(%rdx)
    pop %rax
    vmsave %rax
    pop %rax
    vmload %rax
    pop %rax
    mov 8(%rax), %rdi
    mov (%rax), %rsi
    call amd64_xstate_switch
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size amd64_svm_vmrun, .-amd64_svm_vmrun
.section .note.GNU-stack,"",@progbits
