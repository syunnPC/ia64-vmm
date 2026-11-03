/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SMP end-to-end image: every CPU
 * enters at smp_test_entry with the VMM's entry register set,
 * which it records in its own slot before it takes the new firmware's real
 * _start: an AP branches to ap_start, the BSP runs ipl_main, which is the
 * test's BSP driver (smp-ap.c). smp_test_handler is the OS_BOOT_RENDEZ
 * handler the APs' ap_start calls. tests/qemu/apps.py runs it. */
#define SMP_ENTRY_WORDS 16          /* smp-ap.c enum entry_word */
#define SMP_HANDLER_WORDS 16        /* smp-ap.c enum handler_word */
.explicit
.text
.align 16
.global smp_test_entry
.proc smp_test_entry
smp_test_entry:
    /* Reset state: physical mode, PSR 0, bank 0. Only r14-r16 change before
     * _start, which needs r1, r8-r10, r12 and the backing store as given. */
    mov r14 = cr.lid
    movl r15 = smp_entry_regs
    ;;
    extr.u r14 = r14,24,8
    ;;
    shl r14 = r14,7             /* SMP_ENTRY_WORDS * 8 */
    ;;
    add r15 = r15,r14
    ;;
    st8 [r15] = r1,8
    ;;
    st8 [r15] = r8,8
    ;;
    st8 [r15] = r9,8
    ;;
    st8 [r15] = r10,8
    ;;
    st8 [r15] = r12,8
    mov r16 = ar.bsp
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.bspstore
    ;;
    st8 [r15] = r16,8
    mov r16 = b0
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.rsc
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.k0
    ;;
    st8 [r15] = r16,8
    mov r16 = psr
    ;;
    st8 [r15] = r16,8
    mov r16 = cr.lid
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.itc
    ;;
    st8 [r15] = r16,8
    mov r16 = 1
    ;;
    st8.rel [r15] = r16         /* E_SEEN last */
    ;;
    br.sptk.many _start
    ;;
.endp smp_test_entry

/* OS_BOOT_RENDEZ handler: record the hand-off state before any instruction
 * changes it, then run smp_ap_round in C on
 * the AP's own stack and return into the firmware loop through b0. */
.align 16
.global smp_test_handler
.proc smp_test_handler
smp_test_handler:
    mov r14 = cr.lid
    movl r15 = smp_handler_regs
    ;;
    extr.u r14 = r14,24,8
    ;;
    shl r14 = r14,7             /* SMP_HANDLER_WORDS * 8 */
    ;;
    add r15 = r15,r14
    mov r16 = b0
    ;;
    st8 [r15] = r1,8
    ;;
    st8 [r15] = r12,8
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.bsp
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.bspstore
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.k1
    ;;
    st8 [r15] = r16,8
    mov r16 = cr.iva
    ;;
    st8 [r15] = r16,8
    mov r16 = cr.tpr
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.rsc
    ;;
    st8 [r15] = r16,8
    mov r16 = psr
    ;;
    st8 [r15] = r16,8
    mov r16 = cr.lid
    ;;
    st8 [r15] = r16,8
    mov r16 = ar.itc
    ;;
    st8 [r15] = r16
    ;;
    /* SDM Vol.3 alloc: first in its instruction group. */
    alloc r2 = ar.pfs,0,2,1,0
    ;;
    mov loc0 = b0
    mov loc1 = r2
    movl gp = __gp
    mov out0 = r14
    ;;
    shr.u out0 = out0,7
    br.call.sptk.many b0 = smp_ap_round
    ;;
    mov b0 = loc0
    mov ar.pfs = loc1
    ;;
    br.ret.sptk.many b0
    ;;
.endp smp_test_handler

/* ptc.g / ptc.ga of one 16 KiB page at in0 in region 0 (the ptc.g guard). */
.align 16
.global smp_test_ptcg, smp_test_ptcga
.proc smp_test_ptcg
smp_test_ptcg:
    mov r14 = 14 << 2
    ;;
    ptc.g r32,r14
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.endp smp_test_ptcg
.proc smp_test_ptcga
smp_test_ptcga:
    mov r14 = 14 << 2
    ;;
    ptc.ga r32,r14
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.endp smp_test_ptcga

/* The firmware allocation must cover the whole 1 MiB code page that the
 * link script's ITR(0) mapping spans. */
.section .rodata
    .space 0x100000
