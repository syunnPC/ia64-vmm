/* SPDX-License-Identifier: GPL-2.0-or-later */
/* selftest=simd (host/amd64/tests/simd.c): the SIMD state harness, the
 * interrupt wait loop and the native x86 guest program. */
#define S_ST 512
#define S_MXCSR 544
#define S_FCW 548
#define S_FSW 550

/*
 * uint64_t amd64_simd_probe_call(const struct simd_state *in,
 *     struct simd_state *out, const void *fn, void *a, void *b, void *c)
 *
 * Loads YMM0-15, MXCSR, the x87 control word and two x87 stack values from
 * in, calls fn(a, b, c), and stores the same state to out before anything
 * else runs: fn and whatever interrupts it must leave all of it as loaded.
 * The caller's MXCSR and x87 control word are restored, the x87 stack is
 * left empty and fn's RAX is returned.
 */
.section .text
.global amd64_simd_probe_call
.type amd64_simd_probe_call,@function
amd64_simd_probe_call:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $24, %rsp
    stmxcsr 0(%rsp)
    fnstcw 4(%rsp)
    mov %rdi, %r12
    mov %rsi, %r13
    mov %rdx, %r14
    mov %rcx, %rdi
    mov %r8, %rsi
    mov %r9, %rdx
    vmovdqu 0(%r12), %ymm0
    vmovdqu 32(%r12), %ymm1
    vmovdqu 64(%r12), %ymm2
    vmovdqu 96(%r12), %ymm3
    vmovdqu 128(%r12), %ymm4
    vmovdqu 160(%r12), %ymm5
    vmovdqu 192(%r12), %ymm6
    vmovdqu 224(%r12), %ymm7
    vmovdqu 256(%r12), %ymm8
    vmovdqu 288(%r12), %ymm9
    vmovdqu 320(%r12), %ymm10
    vmovdqu 352(%r12), %ymm11
    vmovdqu 384(%r12), %ymm12
    vmovdqu 416(%r12), %ymm13
    vmovdqu 448(%r12), %ymm14
    vmovdqu 480(%r12), %ymm15
    ldmxcsr S_MXCSR(%r12)
    fninit
    fldcw S_FCW(%r12)
    fldt S_ST(%r12)
    fldt (S_ST + 16)(%r12)
    call *%r14
    mov %rax, %r15
    vmovdqu %ymm0, 0(%r13)
    vmovdqu %ymm1, 32(%r13)
    vmovdqu %ymm2, 64(%r13)
    vmovdqu %ymm3, 96(%r13)
    vmovdqu %ymm4, 128(%r13)
    vmovdqu %ymm5, 160(%r13)
    vmovdqu %ymm6, 192(%r13)
    vmovdqu %ymm7, 224(%r13)
    vmovdqu %ymm8, 256(%r13)
    vmovdqu %ymm9, 288(%r13)
    vmovdqu %ymm10, 320(%r13)
    vmovdqu %ymm11, 352(%r13)
    vmovdqu %ymm12, 384(%r13)
    vmovdqu %ymm13, 416(%r13)
    vmovdqu %ymm14, 448(%r13)
    vmovdqu %ymm15, 480(%r13)
    stmxcsr S_MXCSR(%r13)
    fnstcw S_FCW(%r13)
    fnstsw S_FSW(%r13)
    fstpt (S_ST + 16)(%r13)
    fstpt S_ST(%r13)
    ldmxcsr 0(%rsp)
    fldcw 4(%rsp)
    vzeroupper
    mov %r15, %rax
    add $24, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size amd64_simd_probe_call, .-amd64_simd_probe_call

/* uint64_t amd64_simd_wait(volatile uint32_t flags[2]): set flags[0], then
 * spin (the caller has interrupts enabled) until another CPU sets flags[1].
 * Touches no SIMD or x87 register. */
.global amd64_simd_wait
.type amd64_simd_wait,@function
amd64_simd_wait:
    movl $1, (%rdi)
1:  pause
    cmpl $0, 4(%rdi)
    je 1b
    xor %eax, %eax
    ret
.size amd64_simd_wait, .-amd64_simd_wait

/*
 * The native guest, loaded at GPA 1000h, data page at 2000h (offsets in
 * simd.c). Real mode with CR4.OSFXSR and OSXSAVE; each HLT ends a phase:
 *  1  record the initial XMM0, MXCSR and FCW, load XMM0-7, MXCSR, FCW and
 *     two x87 values
 *  2  store them back, enter 32-bit protected mode, record XCR0, load YMM0-7
 *  3  store YMM0-7
 *  4  XSETBV(0, 1): must exit to the host
 */
#define GUEST_BASE 0x1000
#define D 0x2000
.section .rodata
.global amd64_simd_guest, amd64_simd_guest_end
amd64_simd_guest:
.code16
    mov $D, %bx
    movdqu %xmm0, 0x2c0(%bx)
    stmxcsr 0x2d0(%bx)
    fnstcw 0x2d4(%bx)
    movdqu 0x00(%bx), %xmm0
    movdqu 0x10(%bx), %xmm1
    movdqu 0x20(%bx), %xmm2
    movdqu 0x30(%bx), %xmm3
    movdqu 0x40(%bx), %xmm4
    movdqu 0x50(%bx), %xmm5
    movdqu 0x60(%bx), %xmm6
    movdqu 0x70(%bx), %xmm7
    ldmxcsr 0x180(%bx)
    fninit
    fldcw 0x184(%bx)
    fldt 0x190(%bx)
    fldt 0x1a0(%bx)
    hlt
    movdqu %xmm0, 0x200(%bx)
    movdqu %xmm1, 0x210(%bx)
    movdqu %xmm2, 0x220(%bx)
    movdqu %xmm3, 0x230(%bx)
    movdqu %xmm4, 0x240(%bx)
    movdqu %xmm5, 0x250(%bx)
    movdqu %xmm6, 0x260(%bx)
    movdqu %xmm7, 0x270(%bx)
    stmxcsr 0x280(%bx)
    fnstcw 0x284(%bx)
    fnstsw 0x286(%bx)
    fstpt 0x2a0(%bx)
    fstpt 0x290(%bx)
    lgdtl 0x420(%bx)
    mov %cr0, %eax
    or $1, %al
    mov %eax, %cr0
    ljmpl $0x08, $(GUEST_BASE + protected - amd64_simd_guest)
.code32
protected:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $D, %ebx
    xor %ecx, %ecx
    xgetbv
    mov %eax, 0x2b0(%ebx)
    mov %edx, 0x2b4(%ebx)
    vmovdqu 0x080(%ebx), %ymm0
    vmovdqu 0x0a0(%ebx), %ymm1
    vmovdqu 0x0c0(%ebx), %ymm2
    vmovdqu 0x0e0(%ebx), %ymm3
    vmovdqu 0x100(%ebx), %ymm4
    vmovdqu 0x120(%ebx), %ymm5
    vmovdqu 0x140(%ebx), %ymm6
    vmovdqu 0x160(%ebx), %ymm7
    hlt
    vmovdqu %ymm0, 0x300(%ebx)
    vmovdqu %ymm1, 0x320(%ebx)
    vmovdqu %ymm2, 0x340(%ebx)
    vmovdqu %ymm3, 0x360(%ebx)
    vmovdqu %ymm4, 0x380(%ebx)
    vmovdqu %ymm5, 0x3a0(%ebx)
    vmovdqu %ymm6, 0x3c0(%ebx)
    vmovdqu %ymm7, 0x3e0(%ebx)
    hlt
    xor %ecx, %ecx
    xor %edx, %edx
    mov $1, %eax
    xsetbv
    hlt
.code64
amd64_simd_guest_end:
.section .note.GNU-stack,"",@progbits
