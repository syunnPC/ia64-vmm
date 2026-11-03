/* SPDX-License-Identifier: GPL-2.0-or-later */
/* initpmi.efi's OS_INIT, OS_MCA and SAL PMI handlers, its waiting loop and
 * its TR overlap.
 *
 * OS_INIT and OS_MCA (SAL 245359-005 §4.8, §5.3) enter in physical mode
 * with PSR.ic = 0, bank 0, GR1 the registered gp, GR8-GR12 and PAL's
 * GR16-GR20. They record that state and the min-state area, then return
 * through GR12 (§4.8.1, §5.4): OS_INIT resumes the interrupted context,
 * OS_MCA a new context at initpmi_recover_ip with initpmi_recover_psr.
 * They use GR2-GR7, GR11-GR15 and GR21-GR31, all unspecified on return,
 * and restore the predicates. */
#include "initpmi.h"
    .text
    .align 16
    .global initpmi_os_init#, initpmi_os_init_slave#, initpmi_os_mca#
    .proc initpmi_os_init#
initpmi_os_init:
    mov r3 = REC_INIT
    mov r24 = r0
    br.sptk.many .Lrecord
    ;;
    .endp initpmi_os_init#
    .align 16
    .proc initpmi_os_init_slave#
initpmi_os_init_slave:
    mov r3 = REC_SLAVE
    mov r24 = r0
    br.sptk.many .Lrecord
    ;;
    .endp initpmi_os_init_slave#
    .align 16
    .proc initpmi_os_mca#
initpmi_os_mca:
    mov r3 = REC_MCA
    mov r24 = 1
    ;;
.Lrecord:
    mov r23 = pr
    addl r2 = @ltoffx(initpmi_records#), gp
    ;;
    ld8.mov r2 = [r2], initpmi_records#
    ;;
    add r2 = r2, r3
    ;;
    mov r4 = r2
    ;;
    st8 [r4] = r1, REC_R8 - REC_R1
    ;;
    st8 [r4] = r8, 8
    ;;
    st8 [r4] = r9, 8
    ;;
    st8 [r4] = r10, 8
    ;;
    st8 [r4] = r11, 8
    ;;
    st8 [r4] = r12, 8
    ;;
    st8 [r4] = r16, 8
    ;;
    st8 [r4] = r17, 8
    ;;
    st8 [r4] = r18, 8
    ;;
    st8 [r4] = r19, 8
    ;;
    st8 [r4] = r20, 8
    mov r5 = psr
    ;;
    st8 [r4] = r5, 8
    mov r5 = ar.rsc
    ;;
    st8 [r4] = r5, 8
    mov r5 = cr.lid
    ;;
    st8 [r4] = r5
    /* The architected part of the min-state area, through UC loads. */
    adds r4 = REC_AREA, r2
    mov r5 = r17
    mov r6 = REC_AREA_WORDS
    ;;
.Lcopy:
    ld8 r7 = [r5], 8
    adds r6 = -1, r6
    ;;
    st8 [r4] = r7, 8
    cmp.ne p6, p7 = r6, r0
    ;;
(p6) br.cond.sptk .Lcopy
    adds r4 = REC_COUNT, r2
    ;;
    ld8 r5 = [r4]
    ;;
    adds r5 = 1, r5
    ;;
    st8.rel [r4] = r5
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt .Lnew_context
    /* OS_INIT: resume the interrupted context (§5.4). */
    mov r22 = r17
    mov r9 = r10
#if INITPMI_BROKEN_RESUME
    mov r10 = 1    /* claims a new context: PAL restores BR1 no more */
#else
    mov r10 = r0
#endif
    mov r8 = r0
    br.sptk.many .Lreturn
    ;;
.Lnew_context:
    /* OS_MCA: the itr ran with PSR.ic = 0, so IIP/IPSR/IFS do not name it;
     * resume a new context at the recovery point instead (SDM Vol.2
     * §11.3.3): the saved state with IIP and IPSR replaced. */
    addl r4 = @ltoffx(initpmi_new_area#), gp
    addl r5 = @ltoffx(initpmi_recover_ip#), gp
    addl r6 = @ltoffx(initpmi_recover_psr#), gp
    ;;
    ld8.mov r4 = [r4], initpmi_new_area#
    ld8.mov r5 = [r5], initpmi_recover_ip#
    ld8.mov r6 = [r6], initpmi_recover_psr#
    ;;
    ld8 r5 = [r5]
    ld8 r6 = [r6]
    mov r22 = r4
    adds r7 = REC_AREA, r2
    mov r11 = REC_AREA_WORDS
    ;;
.Lcopy_new:
    ld8 r12 = [r7], 8
    adds r11 = -1, r11
    ;;
    st8 [r4] = r12, 8
    cmp.ne p6, p7 = r11, r0
    ;;
(p6) br.cond.sptk .Lcopy_new
    adds r7 = 0x198, r22
    adds r11 = 0x1a0, r22
    movl r12 = 0x8000000000000000
    ;;
    st8 [r7] = r5
    st8 [r11] = r6
    or r22 = r22, r12
    adds r12 = REC_R12, r2
    ;;
    ld8 r12 = [r12]
    mov r9 = r10
    mov r10 = 1
    mov r8 = r0
    ;;
.Lreturn:
    mov b0 = r12
    mov pr = r23, -1
    ;;
    br.sptk.many b0
    ;;
    .endp initpmi_os_mca#

/* SAL PMI handler (SDM Vol.2 §11.5.2-§11.5.3): records GR24-GR29, BR0,
 * PSR and RSC in the record that follows it, using only GR30-GR31, and
 * returns through BR0. It must sit on a 256-byte boundary
 * (PAL_PMI_ENTRYPOINT). */
    .align 256
    .global initpmi_sale_pmi#
    .proc initpmi_sale_pmi#
initpmi_sale_pmi:
    mov r30 = ip
    ;;
    adds r30 = initpmi_pmi_record# - initpmi_sale_pmi#, r30
    ;;
    st8 [r30] = r24, 8
    ;;
    st8 [r30] = r25, 8
    ;;
    st8 [r30] = r26, 8
    ;;
    st8 [r30] = r27, 8
    ;;
    st8 [r30] = r28, 8
    ;;
    st8 [r30] = r29, 8
    mov r31 = b0
    ;;
    st8 [r30] = r31, 8
    mov r31 = psr
    ;;
    st8 [r30] = r31, 8
    mov r31 = ar.rsc
    ;;
    st8 [r30] = r31, 8
    ;;
    ld8 r31 = [r30]
    ;;
    adds r31 = 1, r31
    ;;
    st8.rel [r30] = r31
    br.sptk.many b0
    ;;
    .endp initpmi_sale_pmi#
    .align 8
    .global initpmi_pmi_record#
initpmi_pmi_record:
    .skip PMI_SIZE

/* UINT64 initpmi_wait(volatile UINT64 *flag, volatile UINT64 *ready,
 *                     UINT64 ticks, UINT64 ipi_address, UINT64 ipi_data)
 * With PSR.i = 0, holds SIG(n) in GR4-GR7, GR14-GR31 (bank 1), its locals
 * and BR1-BR5 and WAIT_PR in p10-p15, publishes *ready, sends ipi_data to
 * ipi_address (UC) when that is not 0, and waits for *flag to become
 * nonzero or for `ticks` of ITC. Returns a mask of the registers that
 * changed (bit 30: the predicates), with WAIT_TIMEOUT when the flag
 * stayed 0. initpmi_wait_loop..initpmi_wait_end is the waiting code. */
    .align 32
    .global initpmi_wait#, initpmi_wait_loop#, initpmi_wait_end#
    .proc initpmi_wait#
initpmi_wait:
    alloc r2 = ar.pfs, 5, 17, 0, 0
    ;;
    mov loc0 = r2
    mov loc1 = b0
    mov loc2 = psr
    mov loc3 = r4
    mov loc4 = r5
    mov loc5 = r6
    mov loc6 = r7
    mov loc7 = b1
    mov loc8 = b2
    mov loc9 = b3
    mov loc10 = b4
    mov loc11 = b5
    mov loc12 = ar.unat
    mov loc13 = pr
    ;;
    rsm psr.i
    ;;
    movl r4 = SIG(4)
    movl r5 = SIG(5)
    movl r6 = SIG(6)
    movl r7 = SIG(7)
    ;;
    movl r14 = SIG(14)
    movl r15 = SIG(15)
    movl r16 = SIG(16)
    movl r17 = SIG(17)
    ;;
    movl r18 = SIG(18)
    movl r19 = SIG(19)
    movl r20 = SIG(20)
    movl r21 = SIG(21)
    ;;
    movl r22 = SIG(22)
    movl r23 = SIG(23)
    movl r24 = SIG(24)
    movl r25 = SIG(25)
    ;;
    movl r26 = SIG(26)
    movl r27 = SIG(27)
    movl r28 = SIG(28)
    movl r29 = SIG(29)
    ;;
    movl r30 = SIG(30)
    movl r31 = SIG(31)
    movl loc14 = SIG(46)
    movl loc15 = SIG(47)
    ;;
    movl loc16 = SIG(48)
    movl r8 = SIG(1)
    ;;
    mov b1 = r8
    adds r8 = 1, r8
    ;;
    mov b2 = r8
    adds r8 = 1, r8
    ;;
    mov b3 = r8
    adds r8 = 1, r8
    ;;
    mov b4 = r8
    adds r8 = 1, r8
    ;;
    mov b5 = r8
    movl r8 = WAIT_PR
    ;;
    mov pr = r8, WAIT_PR_MASK
    mov r8 = 1
    ;;
    st8.rel [in1] = r8
    cmp.eq p6, p7 = in3, r0
    mov r9 = ar.itc
    ;;
    mf
    ;;
(p7) st8 [in3] = in4
    ;;
initpmi_wait_loop:
    ld8.acq r8 = [in0]
    mov r10 = ar.itc
    ;;
    cmp.ne p6, p7 = r8, r0
    sub r10 = r10, r9
    ;;
(p6) br.cond.dpnt .Lwaited
    cmp.ltu p6, p7 = r10, in2
    ;;
(p6) br.cond.sptk initpmi_wait_loop
    ;;
initpmi_wait_end:
.Lwaited:
    /* p6: the flag is set (from the loop) or the time is up (fall-through). */
    movl r8 = WAIT_TIMEOUT
    ;;
(p6) mov r8 = r0
    .macro SAME reg, n, bit
    movl r9 = SIG(\n)
    ;;
    cmp.ne p8, p9 = \reg, r9
    movl r10 = 1 << \bit
    ;;
(p8) or r8 = r8, r10
    ;;
    .endm
    SAME r4, 4, 0
    SAME r5, 5, 1
    SAME r6, 6, 2
    SAME r7, 7, 3
    SAME r14, 14, 4
    SAME r15, 15, 5
    SAME r16, 16, 6
    SAME r17, 17, 7
    SAME r18, 18, 8
    SAME r19, 19, 9
    SAME r20, 20, 10
    SAME r21, 21, 11
    SAME r22, 22, 12
    SAME r23, 23, 13
    SAME r24, 24, 14
    SAME r25, 25, 15
    SAME r26, 26, 16
    SAME r27, 27, 17
    SAME r28, 28, 18
    SAME r29, 29, 19
    SAME r30, 30, 20
    SAME r31, 31, 21
    SAME loc14, 46, 22
    SAME loc15, 47, 23
    SAME loc16, 48, 24
    mov r11 = b1
    ;;
    SAME r11, 1, 25
    mov r11 = b2
    ;;
    SAME r11, 2, 26
    mov r11 = b3
    ;;
    SAME r11, 3, 27
    mov r11 = b4
    ;;
    SAME r11, 4, 28
    mov r11 = b5
    ;;
    SAME r11, 5, 29
    mov r11 = pr
    movl r10 = WAIT_PR_MASK
    ;;
    and r11 = r11, r10
    movl r9 = WAIT_PR
    ;;
    cmp.ne p8, p9 = r11, r9
    movl r10 = 1 << 30
    ;;
(p8) or r8 = r8, r10
    ;;
    mov r4 = loc3
    mov r5 = loc4
    mov r6 = loc5
    mov r7 = loc6
    mov b1 = loc7
    mov b2 = loc8
    ;;
    mov b3 = loc9
    mov b4 = loc10
    mov b5 = loc11
    mov ar.unat = loc12
    mov pr = loc13, -1
    ;;
    mov psr.l = loc2
    ;;
    srlz.d
    mov b0 = loc1
    mov ar.pfs = loc0
    ;;
    br.ret.sptk.many b0
    ;;
    .endp initpmi_wait#

/* UINT64 initpmi_overlap(UINT64 va, UINT64 pte, UINT64 itir, UINT64 slot,
 *                        UINT64 purge)
 * itr.d dtr[slot] = pte, then with purge 0 the same translation into
 * dtr[slot + 1], a same-stream TR overlap; with purge 1, PSR.ic = 1 again
 * and a ptc.l of the translation's page (initpmi_ptc) followed by srlz.i
 * (initpmi_ptc_srlz). Both must machine check (SDM Vol.2 Table 4-1).
 * OS_MCA resumes at initpmi_recovered with the PSR this function entered
 * with; the result is 1 there and 0 when no machine check came. */
    .align 32
    .global initpmi_overlap#
    .proc initpmi_overlap#
initpmi_overlap:
    alloc r2 = ar.pfs, 5, 2, 0, 0
    ;;
    mov loc0 = r2
    mov loc1 = b0
    addl r14 = @ltoffx(initpmi_recover_psr#), gp
    addl r15 = @ltoffx(initpmi_recover_ip#), gp
    mov r16 = psr
    ;;
    ld8.mov r14 = [r14], initpmi_recover_psr#
    ld8.mov r15 = [r15], initpmi_recover_ip#
    addl r17 = @ltoffx(initpmi_recovered#), gp
    ;;
    ld8.mov r17 = [r17], initpmi_recovered#
    dep r19 = -1, r16, 44, 1    /* mov r=psr reads no PSR.bn; this code runs in bank 1 */
    ;;
    st8 [r14] = r19
    ;;
    st8 [r15] = r17
    adds r18 = 1, in3
    ;;
    rsm psr.ic | psr.i
    ;;
    srlz.d
    ;;
    mov cr.ifa = in0
    mov cr.itir = in2
    ;;
    itr.d dtr[in3] = in1
    ;;
    srlz.d
    cmp.ne p6, p7 = in4, r0
    ;;
(p6) br.cond.dpnt .Lpurge
    ;;
    itr.d dtr[r18] = in1
    ;;
    srlz.d
    mov r8 = r0
    ;;
    ssm psr.ic
    ;;
    srlz.d
    br.sptk.many .Lout
    ;;
.Lpurge:
    ssm psr.ic
    ;;
    srlz.d
    ;;
    .align 16
    .global initpmi_ptc#, initpmi_ptc_srlz#
initpmi_ptc:
    ptc.l in0, in2
    ;;
    .align 16
initpmi_ptc_srlz:
    srlz.i
    ;;
    mov r8 = r0
    br.sptk.many .Lout
    ;;
    .global initpmi_recovered#
initpmi_recovered:
    mov r8 = 1
    ;;
.Lout:
    mov psr.l = r16
    ;;
    srlz.d
    mov b0 = loc1
    mov ar.pfs = loc0
    ;;
    br.ret.sptk.many b0
    ;;
    .endp initpmi_overlap#

/* void initpmi_purge(UINT64 va, UINT64 ps): ptr.d of the overlap's DTRs. */
    .align 32
    .global initpmi_purge#
    .proc initpmi_purge#
initpmi_purge:
    shl r14 = in1, 2
    ;;
    ptr.d in0, r14
    ;;
    srlz.d
    br.ret.sptk.many b0
    ;;
    .endp initpmi_purge#

    .data
    .align 4096
    .global initpmi_new_area#
initpmi_new_area:
    .skip 4096
    .align 8
    .global initpmi_records#, initpmi_recover_ip#, initpmi_recover_psr#
initpmi_records:
    .skip REC_RECORDS
initpmi_recover_ip:
    .skip 8
initpmi_recover_psr:
    .skip 8
