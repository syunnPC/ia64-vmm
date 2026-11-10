/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * int amd64_vmx_vmrun(struct vmx_gprs *g, int launched, const struct amd64_xstate_pair *x)
 *
 * One VM entry of the real-mode container. Caller: RFLAGS.IF clear, the container's VMCS current, every
 * VMCS field but HOST_RSP written. Returns 0 after a VM exit, 1 on
 * VMfailInvalid (CF=1), 2 on VMfailValid (ZF=1; the VM-instruction error
 * field says why, SDM Vol.3C §33.2 and Table 33-1).
 *
 * The VMCS holds the guest's RSP, RIP and RFLAGS but no other general
 * register: they are loaded here before VMLAUNCH/VMRESUME and stored at
 * the exit landing point (HOST_RIP = amd64_vmx_exit), where RSP is the
 * value written to HOST_RSP. A VM exit loads the host's CR0/CR3/CR4,
 * segment registers, FS/GS/TR bases, GDTR/IDTR bases, SYSENTER MSRs and
 * EFER from the host-state area, but sets the GDTR and IDTR limits to
 * FFFFh (§30.5.2): both are reloaded from the pseudo-descriptors saved
 * below before anything can use them. The TR limit becomes 67h, which is
 * the VMM's TSS limit already (host/amd64/cpu.c, struct tss64, no I/O
 * bitmap); LDTR becomes unusable, and the VMM has no LDT. No x87, SSE or
 * AVX state and no XCR0 change on entry or exit: the host's enabled state
 * is saved to x->host and the guest's loaded from x->guest before the
 * entry; the exit, and a failed entry, restore the host's before any C runs.
 */
#define G_RAX 0
#define G_RBX 8
#define G_RCX 16
#define G_RDX 24
#define G_RSI 32
#define G_RDI 40
#define G_RBP 48
#define G_R8 56
#define G_R9 64
#define G_R10 72
#define G_R11 80
#define G_R12 88
#define G_R13 96
#define G_R14 104
#define G_R15 112
#define VMCS_HOST_RSP 0x6c14
/* Frame below the callee-saved registers: GDTR at 0, IDTR at 16 (10-byte
 * pseudo-descriptors), the gprs pointer at 32, the XSAVE areas at 40. */
#define F_GDTR 0
#define F_IDTR 16
#define F_GPRS 32
#define F_AREAS 40
.section .text
.global amd64_vmx_vmrun
.type amd64_vmx_vmrun,@function
amd64_vmx_vmrun:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp
    push %rdx
    push %rdi
    sub $32, %rsp
    sgdt F_GDTR(%rsp)
    sidt F_IDTR(%rsp)
    mov %esi, %r12d
    mov (%rdx), %rdi
    mov 8(%rdx), %rsi
    cmpq $0, 16(%rdx)
    jne .Lnative_fp_enter
    call amd64_xstate_switch
    jmp .Lfp_entered
.Lnative_fp_enter:
    call amd64_ia32_fp_enter
.Lfp_entered:
    mov F_GPRS(%rsp), %rdi
    mov $VMCS_HOST_RSP, %eax
    vmwrite %rsp, %rax
    jbe 2f
    /* MOV leaves the flags alone: ZF from this test picks the instruction. */
    test %r12d, %r12d
    mov G_RAX(%rdi), %rax
    mov G_RBX(%rdi), %rbx
    mov G_RCX(%rdi), %rcx
    mov G_RDX(%rdi), %rdx
    mov G_RSI(%rdi), %rsi
    mov G_RBP(%rdi), %rbp
    mov G_R8(%rdi), %r8
    mov G_R9(%rdi), %r9
    mov G_R10(%rdi), %r10
    mov G_R11(%rdi), %r11
    mov G_R12(%rdi), %r12
    mov G_R13(%rdi), %r13
    mov G_R14(%rdi), %r14
    mov G_R15(%rdi), %r15
    mov G_RDI(%rdi), %rdi
    jnz 1f
    vmlaunch
    jmp 2f
1:  vmresume
    /* Fall-through: the entry failed before loading guest state. */
2:  mov $1, %ebx
    jc 4f
    mov $2, %ebx
4:  mov F_AREAS(%rsp), %rax
    cmpq $0, 16(%rax)
    jne .Lnative_fp_failed
    mov 8(%rax), %rdi
    mov (%rax), %rsi
    call amd64_xstate_switch
    jmp .Lfp_failed_restored
.Lnative_fp_failed:
    mov (%rax), %rdi
    mov 8(%rax), %rsi
    call amd64_ia32_fp_leave
.Lfp_failed_restored:
    mov %ebx, %eax
    jmp 3f

.global amd64_vmx_exit
amd64_vmx_exit:
    push %rdi
    mov (F_GPRS + 8)(%rsp), %rdi
    mov %rax, G_RAX(%rdi)
    mov %rbx, G_RBX(%rdi)
    mov %rcx, G_RCX(%rdi)
    mov %rdx, G_RDX(%rdi)
    mov %rsi, G_RSI(%rdi)
    mov %rbp, G_RBP(%rdi)
    mov %r8, G_R8(%rdi)
    mov %r9, G_R9(%rdi)
    mov %r10, G_R10(%rdi)
    mov %r11, G_R11(%rdi)
    mov %r12, G_R12(%rdi)
    mov %r13, G_R13(%rdi)
    mov %r14, G_R14(%rdi)
    mov %r15, G_R15(%rdi)
    popq G_RDI(%rdi)
    lgdt F_GDTR(%rsp)
    lidt F_IDTR(%rsp)
    mov F_AREAS(%rsp), %rax
    cmpq $0, 16(%rax)
    jne .Lnative_fp_exit
    mov 8(%rax), %rdi
    mov (%rax), %rsi
    call amd64_xstate_switch
    jmp .Lfp_exit_restored
.Lnative_fp_exit:
    mov (%rax), %rdi
    mov 8(%rax), %rsi
    call amd64_ia32_fp_leave
.Lfp_exit_restored:
    xor %eax, %eax
3:  add $48, %rsp
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size amd64_vmx_vmrun, .-amd64_vmx_vmrun
.section .note.GNU-stack,"",@progbits
