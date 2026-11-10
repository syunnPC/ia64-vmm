/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GIF blocks host delivery during the FP handoffs. Saved host IF=1 at
 * VMRUN permits physical interrupt exits independently of guest IF.
 * Host delivery reopens only after host FP restoration/state export.
 * FNSAVE/FRSTOR carry the local legacy environment without pending #MF.
 * Outer XSAVE/XRSTOR may still discard no-MF pointers/opcode (APM Vol.2
 * 11.5.10); the dispatcher reconstructs them at proven boundaries. */
.section .text
.global amd64_ia32_fp_enter
.type amd64_ia32_fp_enter,@function
amd64_ia32_fp_enter:
    mov 16(%rdi),%rax
    mov %rax,%rdx
    shr $32,%rdx
    mov (%rdi),%rcx
    xsave64 (%rcx)
    testq $1,24(%rdi)
    jz 1f
    /* Admission proved XINUSE.x87=0; restore Table 11-7's initial state,
     * not conditional no-MF error-pointer fields from the XSAVE image. */
    andq $-2,512(%rcx)
1:
    mov 8(%rdi),%rcx
    xrstor64 (%rcx)
    /* Old x87 is initial. FRSTOR may install pending #MF; no waiting x87
     * instruction may execute afterward before original guest execution. */
    frstor 32(%rdi)
    ret
.size amd64_ia32_fp_enter,.-amd64_ia32_fp_enter
.global amd64_ia32_fp_leave
.type amd64_ia32_fp_leave,@function
amd64_ia32_fp_leave:
    fxsave (%rsi)
    fnsave 32(%rdi)
    mov 16(%rdi),%rax
    mov %rax,%rdx
    shr $32,%rdx
    mov (%rdi),%rcx
    xrstor64 (%rcx)
    ret
.size amd64_ia32_fp_leave,.-amd64_ia32_fp_leave
.global amd64_ia32_fp_roundtrip
.type amd64_ia32_fp_roundtrip,@function
amd64_ia32_fp_roundtrip:
    push %r12
    push %r13
    sub $8,%rsp
    mov %rdi,%r12
    mov %rsi,%r13
    call amd64_ia32_fp_enter
    mov %r12,%rdi
    mov %r13,%rsi
    call amd64_ia32_fp_leave
    add $8,%rsp
    pop %r13
    pop %r12
    ret
.size amd64_ia32_fp_roundtrip,.-amd64_ia32_fp_roundtrip
.global amd64_ia32_fp_host_inuse
.type amd64_ia32_fp_host_inuse,@function
amd64_ia32_fp_host_inuse:
    mov $1,%ecx
    xgetbv
    shl $32,%rdx
    or %rdx,%rax
    ret
.size amd64_ia32_fp_host_inuse,.-amd64_ia32_fp_host_inuse
.global amd64_ia32_fp_mxcsr_mask
.type amd64_ia32_fp_mxcsr_mask,@function
amd64_ia32_fp_mxcsr_mask:
    sub $520,%rsp
    fxsave (%rsp)
    mov 28(%rsp),%eax
    add $520,%rsp
    ret
.size amd64_ia32_fp_mxcsr_mask,.-amd64_ia32_fp_mxcsr_mask
.section .note.GNU-stack,"",@progbits
