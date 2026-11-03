/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.S,
 * Source/Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.S
 */
/* One context block per CPU, reached only through ar.k1 (the BSP's
 * control block, an AP's own area: ap.S); no nesting, same RBS: cover/rfi preserve
 * the interrupted CFM; C runs in bank one with a complete static/FP save
 * and TPR.mmi set. SDM Rev 2.3 Vol.2 §6.11.1. Delivery keeps PSR.dt/rt/it
 * (Vol.2 §5.5 item 3): data accesses here clear PSR.dt; the RSE keeps the
 * interrupted translation; a C call from translated code runs on the CPU's
 * own interruption stack (CONTROL_InterruptStack), never the loader's.
 */
#include "ivt-layout.h"
.explicit
.text
.align 16
.global ivt_external
.proc ivt_external
ivt_external:
    /* Delivery retains PSR.dt (SDM Vol.2 §5.5). ar.k1 and this firmware's
     * context/dispatch pointers are physical boot-services addresses. */
    rsm psr.dt
    ;;
    srlz.d
    ;;
    mov r17 = ar.k1
    ;;
    add r18 = 5680,r17
    ;;
    st8 [r18] = r0
    ;;
    br.sptk.many .Lsave_context
    ;;
.global ivt_unaligned
ivt_unaligned:
    /* SDM Vol.2 §5.5.5, Table 5-4: software may defer a control-speculative
     * fault by setting IPSR.ed and retrying. Do this before any context
     * access: PSR.dt survives delivery, and a loader need not map our
     * physical save area. rfi retains the CFM; the CPU supplies NaT and
     * performs the instruction's postupdate. Only bank-zero GRs are scratch.
     * This is also the reference assist's unserviceable-load policy
     * (firmware/vmm_ivt.S vmm_unaligned_unserviceable).
     * ISR.ed (the code page's ITLB.ed) is deliberately not tested. The SDM
     * lets the OS defer a non-fatal exception only when ISR.ed=1, but "may
     * use the same PSR.ed mechanism to defer fatal speculative load
     * exceptions" (Vol.2 §5.5.5; Part II §6.3.1 step 6c). This firmware
     * cannot perform a reference through the interrupted translation (it
     * runs with PSR.dt=0 and owns no loader mappings), so for it the
     * condition is fatal whatever ITLB.ed says. Code without recovery
     * consumes the NaT later and faults in its own context. */
    mov r16 = pr
    mov r17 = cr.isr
    ;;
    tbit.z p6,p7 = r17,36
    ;;
(p6) br.cond.sptk .Lunaligned_save
    ;;
    mov r17 = cr.ipsr
    movl r18 = 1 << 43
    ;;
    or r17 = r17,r18
    ;;
    mov cr.ipsr = r17
    mov pr = r16,-1
    ;;
    rfi
    ;;
.Lunaligned_save:
    mov pr = r16,-1
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
    mov r17 = ar.k1
    mov r19 = 1
    ;;
    add r18 = 5680,r17
    ;;
    st8 [r18] = r19
    ;;
.Lsave_context:
    mov r16 = pr
    ;;
    mov r17 = ar.k1
    ;;
    add r18 = IPF_CONTEXT_Pr, r17
    ;;
    st8 [r18] = r16
    ;;
    mov r19 = ar.unat
    ;;
    add r18 = IPF_CONTEXT_ArUnat, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.pfs
    ;;
    add r18 = IPF_CONTEXT_ArPfs, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.rsc
    ;;
    add r18 = IPF_CONTEXT_ArRsc, r17
    ;;
    st8 [r18] = r19
    ;;
    /* BSPSTORE/RNAT reads require lazy RSE, including when the interrupted
     * application selected eager mode (SDM Rev 2.3 Vol.2 §6.5). */
    mov ar.rsc = r0
    ;;
    srlz.d
    ;;
    mov r19 = ar.bsp
    ;;
    add r18 = IPF_CONTEXT_ArBsp, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.bspstore
    ;;
    add r18 = IPF_CONTEXT_ArBspstore, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.rnat
    ;;
    add r18 = IPF_CONTEXT_ArRnat, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.ccv
    ;;
    add r18 = IPF_CONTEXT_ArCcv, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.fpsr
    ;;
    add r18 = IPF_CONTEXT_ArFpsr, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.lc
    ;;
    add r18 = IPF_CONTEXT_ArLc, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.ec
    ;;
    add r18 = IPF_CONTEXT_ArEc, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.dcr
    ;;
    add r18 = IPF_CONTEXT_CrDcr, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.itm
    ;;
    add r18 = IPF_CONTEXT_CrItm, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.iva
    ;;
    add r18 = IPF_CONTEXT_CrIva, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.pta
    ;;
    add r18 = IPF_CONTEXT_CrPta, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.ipsr
    ;;
    add r18 = IPF_CONTEXT_CrIpsr, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.isr
    ;;
    add r18 = IPF_CONTEXT_CrIsr, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.iip
    ;;
    add r18 = IPF_CONTEXT_CrIip, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.ifa
    ;;
    add r18 = IPF_CONTEXT_CrIfa, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.itir
    ;;
    add r18 = IPF_CONTEXT_CrItir, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.iipa
    ;;
    add r18 = IPF_CONTEXT_CrIipa, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.iim
    ;;
    add r18 = IPF_CONTEXT_CrIim, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = cr.iha
    ;;
    add r18 = IPF_CONTEXT_CrIha, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b0
    ;;
    add r18 = IPF_CONTEXT_B0, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b1
    ;;
    add r18 = IPF_CONTEXT_B1, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b2
    ;;
    add r18 = IPF_CONTEXT_B2, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b3
    ;;
    add r18 = IPF_CONTEXT_B3, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b4
    ;;
    add r18 = IPF_CONTEXT_B4, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b5
    ;;
    add r18 = IPF_CONTEXT_B5, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b6
    ;;
    add r18 = IPF_CONTEXT_B6, r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = b7
    ;;
    add r18 = IPF_CONTEXT_B7, r17
    ;;
    st8 [r18] = r19
    ;;
    add r18 = IPF_CONTEXT_R1, r17
    ;;
    st8.spill [r18] = r1, 8
    ;;
    st8.spill [r18] = r2, 8
    ;;
    st8.spill [r18] = r3, 8
    ;;
    st8.spill [r18] = r4, 8
    ;;
    st8.spill [r18] = r5, 8
    ;;
    st8.spill [r18] = r6, 8
    ;;
    st8.spill [r18] = r7, 8
    ;;
    st8.spill [r18] = r8, 8
    ;;
    st8.spill [r18] = r9, 8
    ;;
    st8.spill [r18] = r10, 8
    ;;
    st8.spill [r18] = r11, 8
    ;;
    st8.spill [r18] = r12, 8
    ;;
    st8.spill [r18] = r13, 8
    ;;
    st8.spill [r18] = r14, 8
    ;;
    st8.spill [r18] = r15, 8
    ;;
    mov r15 = r17
    add r14 = IPF_CONTEXT_R16, r17
    ;;
    bsw.1
    ;;
    st8.spill [r14] = r16, 8
    ;;
    st8.spill [r14] = r17, 8
    ;;
    st8.spill [r14] = r18, 8
    ;;
    st8.spill [r14] = r19, 8
    ;;
    st8.spill [r14] = r20, 8
    ;;
    st8.spill [r14] = r21, 8
    ;;
    st8.spill [r14] = r22, 8
    ;;
    st8.spill [r14] = r23, 8
    ;;
    st8.spill [r14] = r24, 8
    ;;
    st8.spill [r14] = r25, 8
    ;;
    st8.spill [r14] = r26, 8
    ;;
    st8.spill [r14] = r27, 8
    ;;
    st8.spill [r14] = r28, 8
    ;;
    st8.spill [r14] = r29, 8
    ;;
    st8.spill [r14] = r30, 8
    ;;
    st8.spill [r14] = r31, 8
    ;;
    bsw.0
    ;;
    mov r17 = r15
    mov r19 = ar.unat
    ;;
    shr.u r19 = r19, 2
    ;;
    add r18 = IPF_CONTEXT_IntNat, r17
    ;;
    st8 [r18] = r19
    ;;
    add r14 = IPF_CONTEXT_F2, r17
    ;;
    stf.spill [r14] = f2, 16
    ;;
    stf.spill [r14] = f3, 16
    ;;
    stf.spill [r14] = f4, 16
    ;;
    stf.spill [r14] = f5, 16
    ;;
    stf.spill [r14] = f6, 16
    ;;
    stf.spill [r14] = f7, 16
    ;;
    stf.spill [r14] = f8, 16
    ;;
    stf.spill [r14] = f9, 16
    ;;
    stf.spill [r14] = f10, 16
    ;;
    stf.spill [r14] = f11, 16
    ;;
    stf.spill [r14] = f12, 16
    ;;
    stf.spill [r14] = f13, 16
    ;;
    stf.spill [r14] = f14, 16
    ;;
    stf.spill [r14] = f15, 16
    ;;
    stf.spill [r14] = f16, 16
    ;;
    stf.spill [r14] = f17, 16
    ;;
    stf.spill [r14] = f18, 16
    ;;
    stf.spill [r14] = f19, 16
    ;;
    stf.spill [r14] = f20, 16
    ;;
    stf.spill [r14] = f21, 16
    ;;
    stf.spill [r14] = f22, 16
    ;;
    stf.spill [r14] = f23, 16
    ;;
    stf.spill [r14] = f24, 16
    ;;
    stf.spill [r14] = f25, 16
    ;;
    stf.spill [r14] = f26, 16
    ;;
    stf.spill [r14] = f27, 16
    ;;
    stf.spill [r14] = f28, 16
    ;;
    stf.spill [r14] = f29, 16
    ;;
    stf.spill [r14] = f30, 16
    ;;
    stf.spill [r14] = f31, 16
    ;;
    movl r19 = 4096
    ;;
    add r14 = r19, r17
    ;;
    stf.spill [r14] = f32, 16
    ;;
    stf.spill [r14] = f33, 16
    ;;
    stf.spill [r14] = f34, 16
    ;;
    stf.spill [r14] = f35, 16
    ;;
    stf.spill [r14] = f36, 16
    ;;
    stf.spill [r14] = f37, 16
    ;;
    stf.spill [r14] = f38, 16
    ;;
    stf.spill [r14] = f39, 16
    ;;
    stf.spill [r14] = f40, 16
    ;;
    stf.spill [r14] = f41, 16
    ;;
    stf.spill [r14] = f42, 16
    ;;
    stf.spill [r14] = f43, 16
    ;;
    stf.spill [r14] = f44, 16
    ;;
    stf.spill [r14] = f45, 16
    ;;
    stf.spill [r14] = f46, 16
    ;;
    stf.spill [r14] = f47, 16
    ;;
    stf.spill [r14] = f48, 16
    ;;
    stf.spill [r14] = f49, 16
    ;;
    stf.spill [r14] = f50, 16
    ;;
    stf.spill [r14] = f51, 16
    ;;
    stf.spill [r14] = f52, 16
    ;;
    stf.spill [r14] = f53, 16
    ;;
    stf.spill [r14] = f54, 16
    ;;
    stf.spill [r14] = f55, 16
    ;;
    stf.spill [r14] = f56, 16
    ;;
    stf.spill [r14] = f57, 16
    ;;
    stf.spill [r14] = f58, 16
    ;;
    stf.spill [r14] = f59, 16
    ;;
    stf.spill [r14] = f60, 16
    ;;
    stf.spill [r14] = f61, 16
    ;;
    stf.spill [r14] = f62, 16
    ;;
    stf.spill [r14] = f63, 16
    ;;
    stf.spill [r14] = f64, 16
    ;;
    stf.spill [r14] = f65, 16
    ;;
    stf.spill [r14] = f66, 16
    ;;
    stf.spill [r14] = f67, 16
    ;;
    stf.spill [r14] = f68, 16
    ;;
    stf.spill [r14] = f69, 16
    ;;
    stf.spill [r14] = f70, 16
    ;;
    stf.spill [r14] = f71, 16
    ;;
    stf.spill [r14] = f72, 16
    ;;
    stf.spill [r14] = f73, 16
    ;;
    stf.spill [r14] = f74, 16
    ;;
    stf.spill [r14] = f75, 16
    ;;
    stf.spill [r14] = f76, 16
    ;;
    stf.spill [r14] = f77, 16
    ;;
    stf.spill [r14] = f78, 16
    ;;
    stf.spill [r14] = f79, 16
    ;;
    stf.spill [r14] = f80, 16
    ;;
    stf.spill [r14] = f81, 16
    ;;
    stf.spill [r14] = f82, 16
    ;;
    stf.spill [r14] = f83, 16
    ;;
    stf.spill [r14] = f84, 16
    ;;
    stf.spill [r14] = f85, 16
    ;;
    stf.spill [r14] = f86, 16
    ;;
    stf.spill [r14] = f87, 16
    ;;
    stf.spill [r14] = f88, 16
    ;;
    stf.spill [r14] = f89, 16
    ;;
    stf.spill [r14] = f90, 16
    ;;
    stf.spill [r14] = f91, 16
    ;;
    stf.spill [r14] = f92, 16
    ;;
    stf.spill [r14] = f93, 16
    ;;
    stf.spill [r14] = f94, 16
    ;;
    stf.spill [r14] = f95, 16
    ;;
    stf.spill [r14] = f96, 16
    ;;
    stf.spill [r14] = f97, 16
    ;;
    stf.spill [r14] = f98, 16
    ;;
    stf.spill [r14] = f99, 16
    ;;
    stf.spill [r14] = f100, 16
    ;;
    stf.spill [r14] = f101, 16
    ;;
    stf.spill [r14] = f102, 16
    ;;
    stf.spill [r14] = f103, 16
    ;;
    stf.spill [r14] = f104, 16
    ;;
    stf.spill [r14] = f105, 16
    ;;
    stf.spill [r14] = f106, 16
    ;;
    stf.spill [r14] = f107, 16
    ;;
    stf.spill [r14] = f108, 16
    ;;
    stf.spill [r14] = f109, 16
    ;;
    stf.spill [r14] = f110, 16
    ;;
    stf.spill [r14] = f111, 16
    ;;
    stf.spill [r14] = f112, 16
    ;;
    stf.spill [r14] = f113, 16
    ;;
    stf.spill [r14] = f114, 16
    ;;
    stf.spill [r14] = f115, 16
    ;;
    stf.spill [r14] = f116, 16
    ;;
    stf.spill [r14] = f117, 16
    ;;
    stf.spill [r14] = f118, 16
    ;;
    stf.spill [r14] = f119, 16
    ;;
    stf.spill [r14] = f120, 16
    ;;
    stf.spill [r14] = f121, 16
    ;;
    stf.spill [r14] = f122, 16
    ;;
    stf.spill [r14] = f123, 16
    ;;
    stf.spill [r14] = f124, 16
    ;;
    stf.spill [r14] = f125, 16
    ;;
    stf.spill [r14] = f126, 16
    ;;
    stf.spill [r14] = f127, 16
    ;;
    cover
    ;;
    mov r19 = cr.ifs
    ;;
    add r18 = IPF_CONTEXT_CrIfs, r17
    ;;
    st8 [r18] = r19
    ;;
    add r18 = 5680,r17
    ;;
    ld8 r19 = [r18]
    ;;
    cmp.eq p6,p7 = r19,r0
    ;;
(p6) br.cond.sptk .Lrse_saved
    ;;
    /* cover saved the interrupted CFM. Spill it before the C emulator
     * uses stacked registers; rfi must refill any edited backing slots. */
    flushrs
    ;;
    mov r19 = ar.bspstore
    ;;
    add r18 = CONTROL_RseBspstore,r17
    ;;
    st8 [r18] = r19
    ;;
    mov r19 = ar.rnat
    ;;
    add r18 = CONTROL_RseRnat,r17
    ;;
    st8 [r18] = r19
    ;;
.Lrse_saved:
    mov r19 = cr.tpr
    ;;
    add r18 = 5632, r17
    ;;
    st8 [r18] = r19
    ;;
    add r18 = 5680,r17
    ;;
    ld8 r19 = [r18]
    ;;
    cmp.eq p6,p7 = r19,r0
    ;;
(p6) mov r20 = cr.ivr
(p7) mov r20 = 0x5a
    ;;
    srlz.d
    ;;
    add r18 = 5640, r17
    ;;
    st8 [r18] = r20
    ;;
    cmp.eq p6,p7 = 15,r20
    ;;
(p6) br.cond.sptk .Lrestore
    ;;
    movl r19 = 0x10000
    ;;
    mov cr.tpr = r19
    ;;
    srlz.d
    ;;
    mov r4 = r17
    add r18 = 5680,r17
    ;;
    ld8 r19 = [r18]
    ;;
    cmp.ne p6,p7 = r19,r0
    ;;
(p6) add r17 = 5688,r17
    ;;
    ld8 r19 = [r17],8
    ;;
    ld8 r1 = [r17]
    ;;
    mov b6 = r19
    /* An interrupted r12 is a virtual address when IPSR.dt=1; with DT now
     * clear, using it would write the C frames to whatever physical memory
     * has that number (or fault in region 7). Such calls run on this CPU's
     * interruption stack (ipl.c, ap.c); the GR restore below reloads the
     * interrupted r12. IPSR.dt=0 means firmware or a physical-mode loader,
     * whose r12 is already physical. */
    add r20 = IPF_CONTEXT_CrIpsr,r4
    ;;
    ld8 r20 = [r20]
    add r21 = CONTROL_InterruptStack,r4
    ;;
    tbit.nz p6,p7 = r20,17
    ;;
(p6) ld8 r12 = [r21]
    ;;
    add r12 = -64,r12
    ;;
    bsw.1
    ssm psr.ic
    ;;
    srlz.d
    ;;
    alloc r2 = ar.pfs,0,0,2,0
    ;;
    add r20 = 5640,r4
    ;;
    ld8 r32 = [r20]
    add r33 = 16,r4
    ;;
    br.call.sptk.many b0 = b6
    ;;
    rsm psr.i
    ;;
    srlz.d
    ;;
    alloc r2 = ar.pfs,0,0,0,0
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    bsw.0
    ;;
    mov r17 = r4
    ;;
    add r18 = 5680,r17
    ;;
    ld8 r19 = [r18]
    ;;
    cmp.eq p6,p7 = r19,r0
    ;;
(p6) mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lrestore:
    add r18 = 5680,r17
    ;;
    ld8 r19 = [r18]
    ;;
    cmp.eq p6,p7 = r19,r0
    ;;
(p6) br.cond.sptk .Lrnat_restored
    ;;
    /* Backstop: the RNAT address below is an RSE address, virtual when
     * IPSR.rt=1, and DT is clear. The dispatcher never returns in that
     * state (unaligned.c rejects rt=1 first). */
    add r18 = IPF_CONTEXT_CrIpsr,r17
    ;;
    ld8 r19 = [r18]
    ;;
    tbit.nz p6,p7 = r19,27
    ;;
(p6) br.cond.sptk .Lrnat_restored
    ;;
    /* C may have spilled another collection. Commit the edited RNAT to
     * memory or the live collection, according to the current BSPSTORE. */
    add r18 = CONTROL_RseBspstore,r17
    ;;
    ld8 r19 = [r18],8
    ;;
    ld8 r20 = [r18]
    mov r21 = ar.bspstore
    ;;
    mov r22 = 0x1f8
    ;;
    or r19 = r22,r19
    ;;
    cmp.ltu p6,p7 = r19,r21
    ;;
(p6) st8 [r19] = r20
(p7) mov ar.rnat = r20
    ;;
.Lrnat_restored:
    add r18 = 5632,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov cr.tpr = r19
    ;;
    srlz.d
    ;;
    invala
    ;;
    add r14 = IPF_CONTEXT_F2,r17
    ;;
    ldf.fill f2 = [r14],16
    ;;
    ldf.fill f3 = [r14],16
    ;;
    ldf.fill f4 = [r14],16
    ;;
    ldf.fill f5 = [r14],16
    ;;
    ldf.fill f6 = [r14],16
    ;;
    ldf.fill f7 = [r14],16
    ;;
    ldf.fill f8 = [r14],16
    ;;
    ldf.fill f9 = [r14],16
    ;;
    ldf.fill f10 = [r14],16
    ;;
    ldf.fill f11 = [r14],16
    ;;
    ldf.fill f12 = [r14],16
    ;;
    ldf.fill f13 = [r14],16
    ;;
    ldf.fill f14 = [r14],16
    ;;
    ldf.fill f15 = [r14],16
    ;;
    ldf.fill f16 = [r14],16
    ;;
    ldf.fill f17 = [r14],16
    ;;
    ldf.fill f18 = [r14],16
    ;;
    ldf.fill f19 = [r14],16
    ;;
    ldf.fill f20 = [r14],16
    ;;
    ldf.fill f21 = [r14],16
    ;;
    ldf.fill f22 = [r14],16
    ;;
    ldf.fill f23 = [r14],16
    ;;
    ldf.fill f24 = [r14],16
    ;;
    ldf.fill f25 = [r14],16
    ;;
    ldf.fill f26 = [r14],16
    ;;
    ldf.fill f27 = [r14],16
    ;;
    ldf.fill f28 = [r14],16
    ;;
    ldf.fill f29 = [r14],16
    ;;
    ldf.fill f30 = [r14],16
    ;;
    ldf.fill f31 = [r14],16
    ;;
    movl r19 = 4096
    ;;
    add r14 = r19,r17
    ;;
    ldf.fill f32 = [r14],16
    ;;
    ldf.fill f33 = [r14],16
    ;;
    ldf.fill f34 = [r14],16
    ;;
    ldf.fill f35 = [r14],16
    ;;
    ldf.fill f36 = [r14],16
    ;;
    ldf.fill f37 = [r14],16
    ;;
    ldf.fill f38 = [r14],16
    ;;
    ldf.fill f39 = [r14],16
    ;;
    ldf.fill f40 = [r14],16
    ;;
    ldf.fill f41 = [r14],16
    ;;
    ldf.fill f42 = [r14],16
    ;;
    ldf.fill f43 = [r14],16
    ;;
    ldf.fill f44 = [r14],16
    ;;
    ldf.fill f45 = [r14],16
    ;;
    ldf.fill f46 = [r14],16
    ;;
    ldf.fill f47 = [r14],16
    ;;
    ldf.fill f48 = [r14],16
    ;;
    ldf.fill f49 = [r14],16
    ;;
    ldf.fill f50 = [r14],16
    ;;
    ldf.fill f51 = [r14],16
    ;;
    ldf.fill f52 = [r14],16
    ;;
    ldf.fill f53 = [r14],16
    ;;
    ldf.fill f54 = [r14],16
    ;;
    ldf.fill f55 = [r14],16
    ;;
    ldf.fill f56 = [r14],16
    ;;
    ldf.fill f57 = [r14],16
    ;;
    ldf.fill f58 = [r14],16
    ;;
    ldf.fill f59 = [r14],16
    ;;
    ldf.fill f60 = [r14],16
    ;;
    ldf.fill f61 = [r14],16
    ;;
    ldf.fill f62 = [r14],16
    ;;
    ldf.fill f63 = [r14],16
    ;;
    ldf.fill f64 = [r14],16
    ;;
    ldf.fill f65 = [r14],16
    ;;
    ldf.fill f66 = [r14],16
    ;;
    ldf.fill f67 = [r14],16
    ;;
    ldf.fill f68 = [r14],16
    ;;
    ldf.fill f69 = [r14],16
    ;;
    ldf.fill f70 = [r14],16
    ;;
    ldf.fill f71 = [r14],16
    ;;
    ldf.fill f72 = [r14],16
    ;;
    ldf.fill f73 = [r14],16
    ;;
    ldf.fill f74 = [r14],16
    ;;
    ldf.fill f75 = [r14],16
    ;;
    ldf.fill f76 = [r14],16
    ;;
    ldf.fill f77 = [r14],16
    ;;
    ldf.fill f78 = [r14],16
    ;;
    ldf.fill f79 = [r14],16
    ;;
    ldf.fill f80 = [r14],16
    ;;
    ldf.fill f81 = [r14],16
    ;;
    ldf.fill f82 = [r14],16
    ;;
    ldf.fill f83 = [r14],16
    ;;
    ldf.fill f84 = [r14],16
    ;;
    ldf.fill f85 = [r14],16
    ;;
    ldf.fill f86 = [r14],16
    ;;
    ldf.fill f87 = [r14],16
    ;;
    ldf.fill f88 = [r14],16
    ;;
    ldf.fill f89 = [r14],16
    ;;
    ldf.fill f90 = [r14],16
    ;;
    ldf.fill f91 = [r14],16
    ;;
    ldf.fill f92 = [r14],16
    ;;
    ldf.fill f93 = [r14],16
    ;;
    ldf.fill f94 = [r14],16
    ;;
    ldf.fill f95 = [r14],16
    ;;
    ldf.fill f96 = [r14],16
    ;;
    ldf.fill f97 = [r14],16
    ;;
    ldf.fill f98 = [r14],16
    ;;
    ldf.fill f99 = [r14],16
    ;;
    ldf.fill f100 = [r14],16
    ;;
    ldf.fill f101 = [r14],16
    ;;
    ldf.fill f102 = [r14],16
    ;;
    ldf.fill f103 = [r14],16
    ;;
    ldf.fill f104 = [r14],16
    ;;
    ldf.fill f105 = [r14],16
    ;;
    ldf.fill f106 = [r14],16
    ;;
    ldf.fill f107 = [r14],16
    ;;
    ldf.fill f108 = [r14],16
    ;;
    ldf.fill f109 = [r14],16
    ;;
    ldf.fill f110 = [r14],16
    ;;
    ldf.fill f111 = [r14],16
    ;;
    ldf.fill f112 = [r14],16
    ;;
    ldf.fill f113 = [r14],16
    ;;
    ldf.fill f114 = [r14],16
    ;;
    ldf.fill f115 = [r14],16
    ;;
    ldf.fill f116 = [r14],16
    ;;
    ldf.fill f117 = [r14],16
    ;;
    ldf.fill f118 = [r14],16
    ;;
    ldf.fill f119 = [r14],16
    ;;
    ldf.fill f120 = [r14],16
    ;;
    ldf.fill f121 = [r14],16
    ;;
    ldf.fill f122 = [r14],16
    ;;
    ldf.fill f123 = [r14],16
    ;;
    ldf.fill f124 = [r14],16
    ;;
    ldf.fill f125 = [r14],16
    ;;
    ldf.fill f126 = [r14],16
    ;;
    ldf.fill f127 = [r14],16
    ;;
    add r18 = IPF_CONTEXT_ArPfs,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.pfs = r19
    ;;
    add r18 = IPF_CONTEXT_ArRsc,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.rsc = r19
    ;;
    add r18 = IPF_CONTEXT_ArCcv,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.ccv = r19
    ;;
    add r18 = IPF_CONTEXT_ArFpsr,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.fpsr = r19
    ;;
    add r18 = IPF_CONTEXT_ArLc,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.lc = r19
    ;;
    add r18 = IPF_CONTEXT_ArEc,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.ec = r19
    ;;
    add r18 = IPF_CONTEXT_CrIpsr,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov cr.ipsr = r19
    ;;
    add r18 = IPF_CONTEXT_CrIip,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov cr.iip = r19
    ;;
    add r18 = IPF_CONTEXT_CrIfs,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov cr.ifs = r19
    ;;
    add r18 = IPF_CONTEXT_B0,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b0 = r19
    ;;
    add r18 = IPF_CONTEXT_B1,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b1 = r19
    ;;
    add r18 = IPF_CONTEXT_B2,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b2 = r19
    ;;
    add r18 = IPF_CONTEXT_B3,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b3 = r19
    ;;
    add r18 = IPF_CONTEXT_B4,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b4 = r19
    ;;
    add r18 = IPF_CONTEXT_B5,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b5 = r19
    ;;
    add r18 = IPF_CONTEXT_B6,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b6 = r19
    ;;
    add r18 = IPF_CONTEXT_B7,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov b7 = r19
    ;;
    add r18 = IPF_CONTEXT_IntNat,r17
    ;;
    ld8 r19 = [r18]
    ;;
    shl r19 = r19,2
    ;;
    mov ar.unat = r19
    add r14 = IPF_CONTEXT_R16,r17
    mov r15 = r17
    ;;
    bsw.1
    ;;
    ld8.fill r16 = [r14],8
    ;;
    ld8.fill r17 = [r14],8
    ;;
    ld8.fill r18 = [r14],8
    ;;
    ld8.fill r19 = [r14],8
    ;;
    ld8.fill r20 = [r14],8
    ;;
    ld8.fill r21 = [r14],8
    ;;
    ld8.fill r22 = [r14],8
    ;;
    ld8.fill r23 = [r14],8
    ;;
    ld8.fill r24 = [r14],8
    ;;
    ld8.fill r25 = [r14],8
    ;;
    ld8.fill r26 = [r14],8
    ;;
    ld8.fill r27 = [r14],8
    ;;
    ld8.fill r28 = [r14],8
    ;;
    ld8.fill r29 = [r14],8
    ;;
    ld8.fill r30 = [r14],8
    ;;
    ld8.fill r31 = [r14],8
    ;;
    bsw.0
    ;;
    mov r17 = r15
    ;;
    add r18 = IPF_CONTEXT_R1,r17
    ;;
    ld8.fill r1 = [r18],8
    ;;
    ld8.fill r2 = [r18],8
    ;;
    ld8.fill r3 = [r18],8
    ;;
    ld8.fill r4 = [r18],8
    ;;
    ld8.fill r5 = [r18],8
    ;;
    ld8.fill r6 = [r18],8
    ;;
    ld8.fill r7 = [r18],8
    ;;
    ld8.fill r8 = [r18],8
    ;;
    ld8.fill r9 = [r18],8
    ;;
    ld8.fill r10 = [r18],8
    ;;
    ld8.fill r11 = [r18],8
    ;;
    ld8.fill r12 = [r18],8
    ;;
    ld8.fill r13 = [r18],8
    ;;
    ld8.fill r14 = [r18],8
    ;;
    ld8.fill r15 = [r18],8
    ;;
    /* Rearm the timer only after the expensive context restore. Otherwise a
     * slow interpreter can already be overdue at rfi and immediately enter
     * another IRQ without retiring any interrupted instructions. */
    add r18 = 5640,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov r18 = 0xe8
    ;;
    cmp.ne p6,p7 = r18,r19
    ;;
(p6) br.cond.sptk .Larmed
    ;;
    add r18 = 5664,r17
    ;;
    ld8 r20 = [r18]
    ;;
    cmp.eq p6,p7 = r20,r0
    ;;
(p6) br.cond.sptk .Larmed
    ;;
.Larm:
    mov r16 = ar.itc
    ;;
    add r19 = r16,r20
    ;;
    mov cr.itm = r19
    ;;
    srlz.d
    ;;
    mov r16 = ar.itc
    ;;
    sub r19 = r19,r16
    ;;
    cmp.le p6,p7 = r19,r0
    ;;
(p6) br.cond.dpnt .Larm
    ;;
.Larmed:
    add r18 = IPF_CONTEXT_ArUnat,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov ar.unat = r19
    ;;
    add r18 = IPF_CONTEXT_Pr,r17
    ;;
    ld8 r19 = [r18]
    ;;
    mov pr = r19,-1
    ;;
    rfi
    ;;
.endp ivt_external

/* Loader contract: region-zero identity translations, 4 KiB, PL0 RWX,
 * WB for physical RAM and UC for the service window. Other regions fail;
 * Other loaders must install their own IVA/mappings before nonidentity VA.
 * Bank-zero alternate-refill sequence. */
.macro identity instruction
    mov r16 = pr
    mov r17 = cr.ifa
    ;;
    extr.u r18 = r17,50,14
    ;;
    cmp.ne p6,p7 = r18,r0
    ;;
(p6) br.cond.dpnt ivt_fatal
    ;;
    dep r18 = r0,r17,0,12
    movl r19 = 0x0003ffff00000000
    ;;
    cmp.geu p6,p7 = r18,r19
    mov r19 = 0x661
    ;;
(p6) add r19 = 0x10,r19
    ;;
    or r18 = r18,r19
    mov r19 = 48
    ;;
    mov cr.itir = r19
    ;;
.if \instruction
    itc.i r18
    ;;
    srlz.i
.else
    itc.d r18
    ;;
    srlz.d
.endif
    ;;
    mov pr = r16,-1
    ;;
    rfi
    ;;
.endm
.align 16
ivt_identity_i:
    identity 1
ivt_identity_d:
    identity 0
.global ivt_fatal
ivt_fatal:
    /* As in firmware/vmm_ivt.S: fatal delivery can retain PSR.dt, with
     * no mapping for the service window. Report through physical MMIO. */
    rsm psr.dt
    ;;
    srlz.d
    ;;
    mov r16 = cr.iip
    movl r17 = 0x0003fffff8000048
    ;;
    st8 [r17] = r16
    mov r16 = cr.isr
    ;;
    st8 [r17] = r16
    mov r16 = cr.ipsr
    ;;
    st8 [r17] = r16
    mov r16 = cr.ifa
    ;;
    st8 [r17] = r16
    add r17 = -8,r17
    movl r16 = 0x47324956
    ;;
    st8 [r17] = r16
    ;;
    br.sptk ivt_fatal
    ;;
.section .text.ivt,"ax"
.balign 0x8000
.global ivt_base
ivt_base:
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+256
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+512
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+768
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+1024
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+1280
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+1536
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+1792
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+2048
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+2304
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+2560
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+2816
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+3072
    br.sptk.many ivt_identity_i
    ;;
    .org ivt_base+3328
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+3584
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+3840
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+4096
    br.sptk.many ivt_identity_d
    ;;
    .org ivt_base+4352
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+4608
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+4864
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+5120
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+5376
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+5632
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+5888
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+6144
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+6400
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+6656
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+6912
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+7168
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+7424
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+7680
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+7936
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+8192
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+8448
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+8704
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+8960
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+9216
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+9472
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+9728
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+9984
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+10240
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+10496
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+10752
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+11008
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+11264
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+11520
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+11776
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+12032
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+12288
    br.sptk.many ivt_external
    ;;
    .org ivt_base+12544
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+12800
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+13056
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+13312
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+13568
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+13824
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+14080
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+14336
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+14592
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+14848
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+15104
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+15360
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+15616
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+15872
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+16128
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+16384
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+16640
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+16896
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+17152
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+17408
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+17664
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+17920
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+18176
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+18432
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+18688
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+18944
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+19200
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+19456
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+19712
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+19968
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+20224
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+20480
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+20736
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+20992
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+21248
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+21504
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+21760
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+22016
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+22272
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+22528
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+22784
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+23040
    br.sptk.many ivt_unaligned
    ;;
    .org ivt_base+23296
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+23552
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+23808
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+24064
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+24320
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+24576
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+24832
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+25088
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+25344
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+25600
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+25856
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+26112
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+26368
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+26624
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+26880
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+27136
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+27392
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+27648
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+27904
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+28160
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+28416
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+28672
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+28928
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+29184
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+29440
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+29696
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+29952
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+30208
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+30464
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+30720
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+30976
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+31232
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+31488
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+31744
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+32000
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+32256
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+32512
    br.sptk.many ivt_fatal
    ;;
    .org ivt_base+32768
