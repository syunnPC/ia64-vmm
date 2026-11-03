/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAL boot rendezvous of an application processor: SAL 245359-005 §3.2.2.1
 * and §3.2.6. Follows the reference
 * firmware's firmware_ap_main, fw_ap_rendezvous and fw_call_ap_rendezvous.
 * An AP arrives from _start at its first wake (reset-hold) in physical
 * mode with the reset PSR, the wake vector still in IRR, and never leaves
 * this file except into the OS handler, ap.c and the native PAL.
 */
#include "ap.h"
#include "sal-handoff.h"
/* The area base is also the IVT context block (ar.k1) below. */
.if PERCPU_CONTEXT
.error "ap_start expects the context block at the start of the area"
.endif
.explicit
.text
.align 16
.global ap_start
.proc ap_start
ap_start:
    movl r2 = ivt_base
    /* SAL §3.2.5: the OS sees PSR.dfl/dfh as delivered by reset. */
    mov r3 = psr
    ;;
    mov cr.iva = r2
    ;;
    srlz.i
    ;;
    /* SDM Vol.2 §11.10.2.2.3: PAL is called with PSR.bn=1; SAL §3.2.5
     * hands off in bank one as well. */
    bsw.1
    ;;
    mov ar.rsc = r0
    movl r2 = ap_wake_block
    mov r8 = cr.lid
    ;;
    /* Invalidate the entry frame: the VMM's entry stack and backing store
     * are not used (SDM Vol.2 §6.5.4, RSC.loadrs = 0). */
    loadrs
    add r9 = AP_WAKE_COUNT,r2
    extr.u r10 = r8,16,8
    ;;
    ld4.acq r9 = [r9]
    extr.u r11 = r8,24,8
    ;;
    /* eid is 0 and id 0 is the BSP. An AP woken before DXE
     * published its area stops rather than run on a shared or absent stack. */
    cmp.ne p6,p0 = r10,r0
    cmp.eq p7,p0 = r11,r0
    cmp.geu p8,p0 = r11,r9
    ;;
(p6) br.cond.spnt.many .Learly
(p7) br.cond.spnt.many .Learly
(p8) br.cond.spnt.many .Learly
    add r9 = AP_WAKE_BASE,r2
    add r10 = AP_WAKE_STRIDE,r2
    adds r11 = -1,r11
    ;;
    ld8 r9 = [r9]
    ld4 r10 = [r10]
    mov ar.lc = r11
    ;;
.Lindex:
    add r9 = r9,r10
    br.cloop.sptk.few .Lindex
    ;;
    movl r10 = PERCPU_RSE
    movl r11 = PERCPU_STACK+PERCPU_STACK_SIZE-16
    ;;
    add r10 = r10,r9
    add r12 = r11,r9
    ;;
    mov ar.bspstore = r10
    ;;
    /* RNAT is undefined after a BSPSTORE write (SDM Vol.2 §6.5.3). */
    mov ar.rnat = r0
    mov ar.k1 = r9
    ;;
    alloc r2 = ar.pfs,0,3,1,0
    movl r14 = 0x2008
    movl r15 = 0xc0000
    ;;
    /* loc1: OS hand-off PSR.l, ac | ic | reset dfl/dfh (SAL §3.2.5, the
     * reference's sal_loader_psr_low); loc2: this loop's PSR.l. */
    and r15 = r15,r3
    mov loc0 = r9
    mov loc2 = r3
    mov out0 = r9
    movl gp = __gp
    ;;
    or loc1 = r14,r15
    ;;
    br.call.sptk.many b0 = ap_context_init
    ;;
.Lwait:
    /* SAL §3.2.2.1: read IVR, issue EOI, then act on the vector. */
    mov r14 = cr.ivr
    ;;
    srlz.d
    ;;
    mov cr.eoi = r0
    mov r15 = 0xff
    ;;
    srlz.d
    cmp.ne p6,p0 = r14,r15
    movl r14 = ap_wake_block
    ;;
(p6) br.cond.sptk.many .Lidle
    ld8.acq r15 = [r14],AP_WAKE_GP
    ;;
    cmp.eq p6,p0 = r15,r0
    ;;
(p6) br.cond.spnt.many .Lidle
    /* The hand-off state is established anew for every rendezvous; an
     * earlier handler that returned may have changed it. */
    SAL_HANDOFF_REGS r16, r17
    movl gp = __gp
    movl r14 = PERCPU_STACK+PERCPU_STACK_SIZE-16
    ;;
    add r12 = r14,loc0
    ;;
    br.call.sptk.many b0 = ap_install_translation
    ;;
    movl r14 = ap_wake_block
    ;;
    ld8.acq r15 = [r14],AP_WAKE_GP
    ;;
    ld8 r1 = [r14]
    mov b6 = r15
    movl r14 = PERCPU_STACK+PERCPU_STACK_SIZE-16
    movl r15 = ivt_base
    ;;
    add r12 = r14,loc0
    mov ar.k1 = loc0
    mov cr.iva = r15
    ;;
    mov psr.l = loc1
    ;;
    srlz.i
    ;;
    mov ar.rsc = r0
    ;;
    /* SAL §3.2.6: b0 returns into this loop. */
    br.call.sptk.many b0 = b6
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    bsw.1
    ;;
    mov psr.l = loc2
    ;;
    srlz.i
    ;;
    mov ar.rsc = r0
    ;;
.Lidle:
    /* TPR is scratch on return from the OS (reference firmware). */
    mov cr.tpr = r0
    ;;
    srlz.d
    mov r28 = 29
    mov r29 = r0
    mov r30 = r0
    mov r31 = r0
    movl r14 = .Lhalted
    movl r15 = pal_proc_entry
    ;;
    mov b0 = r14
    mov b6 = r15
    ;;
    /* PAL_HALT_LIGHT, static convention: br.cond, return through b0. */
    br.sptk.many b6
    ;;
.Lhalted:
    cmp.eq p6,p0 = r8,r0
    ;;
(p6) br.sptk.many .Lwait
    movl r14 = 0x0003fffff8000048
    movl r15 = 0x4733414c
    ;;
    st8 [r14] = r8,-8
    ;;
    st8 [r14] = r15
    br.sptk.many .Lstop
    ;;
.Learly:
    /* Service ABI, include/ia64vmm/platform.h: debug LID and count, panic. */
    movl r14 = 0x0003fffff8000048
    movl r15 = 0x47334157
    ;;
    st8 [r14] = r8
    ;;
    st8 [r14] = r9,-8
    ;;
    st8 [r14] = r15
    ;;
.Lstop:
    hint @pause
    br.sptk.many .Lstop
    ;;
.endp ap_start
