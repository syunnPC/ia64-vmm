/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OS_BOOT_RENDEZ handler of smp.efi.
 *
 * SAL 245359-005 §3.2.6: the firmware calls the registered handler on an
 * AP in physical mode with gp = the registered gp and b0 returning into its
 * rendezvous loop; the firmware also sets PSR.i = 0, bank 1, ar.rsc = 0
 * and r12 on the AP's own stack. To report that state exactly, this stub
 * records it before any instruction can change it: it uses scratch
 * registers only (r14-r27, Software Conventions §5.2), then branches (not
 * calls) to smp_ap_main, so the C handler returns straight to the
 * firmware through the b0 and ar.pfs the firmware passed. */
#include "smp.h"
    .text
    .align 32
    .global smp_ap_entry#
    .type smp_ap_entry#, @function
    .proc smp_ap_entry#
smp_ap_entry:
    mov r16 = psr
    mov r17 = cr.lid
    addl r14 = @ltoffx(smp_raw#), gp
    ;;
    ld8.mov r14 = [r14], smp_raw#
    extr.u r18 = r17, 24, 8
    mov r19 = ar.itc
    ;;
    shl r18 = r18, SMP_RAW_SHIFT
    mov r20 = b0
    mov r21 = ar.bsp
    ;;
    add r14 = r14, r18
    mov r22 = ar.bspstore
    mov r23 = ar.rsc
    ;;
    add r15 = SMP_RAW_GP, r14
    mov r24 = ar.pfs
    mov r25 = cr.tpr
    ;;
    st8 [r14] = r16, SMP_RAW_SP - SMP_RAW_PSR
    st8 [r15] = r1, SMP_RAW_B0 - SMP_RAW_GP
    mov r26 = cr.iva
    ;;
    st8 [r14] = r12, SMP_RAW_BSP - SMP_RAW_SP
    st8 [r15] = r20, SMP_RAW_BSPSTORE - SMP_RAW_B0
    ;;
    st8 [r14] = r21, SMP_RAW_RSC - SMP_RAW_BSP
    st8 [r15] = r22, SMP_RAW_PFS - SMP_RAW_BSPSTORE
    ;;
    st8 [r14] = r23, SMP_RAW_TPR - SMP_RAW_RSC
    st8 [r15] = r24, SMP_RAW_LID - SMP_RAW_PFS
    ;;
    st8 [r14] = r25, SMP_RAW_ITC - SMP_RAW_TPR
    st8 [r15] = r17, SMP_RAW_IVA - SMP_RAW_LID
    ;;
    st8 [r14] = r19, SMP_RAW_ENTRIES - SMP_RAW_ITC
    st8 [r15] = r26
    ;;
    ld8 r27 = [r14]
    ;;
    adds r27 = 1, r27
    ;;
    st8 [r14] = r27
    br.sptk.many smp_ap_main#
    ;;
    .endp smp_ap_entry#

/* Translation helpers of the purge cases. Leaf
 * procedures on their incoming frame (in0.. = r32..), scratch registers and
 * p6-p9 only. Each window with PSR.ic = 0 also has PSR.i = 0, so no
 * interruption can arrive without state collection (SDM Vol.2 §5.5),
 * and touches no memory other than the access under test. */

/* void smp_tc_insert(va, pte, itir): itc.d needs PSR.ic = 0 (Vol.3 itc). */
    .align 32
    .global smp_tc_insert#
    .type smp_tc_insert#, @function
    .proc smp_tc_insert#
smp_tc_insert:
    mov r16 = psr
    ;;
    rsm 0x6000                  /* psr.i | psr.ic */
    ;;
    srlz.d
    ;;
    mov cr.ifa = r32
    mov cr.itir = r34
    ;;
    itc.d r33
    ;;
    srlz.d
    tbit.nz p6, p0 = r16, 13
    tbit.nz p7, p0 = r16, 14
    ;;
(p6) ssm 0x2000
    ;;
(p7) ssm 0x4000
    ;;
    srlz.d
    br.ret.sptk.many b0
    ;;
    .endp smp_tc_insert#

/* UINT64 smp_probe(va, UINT64 *value): 1 and *value when a speculative
 * load through the data translation of va completes, 0 when it is
 * deferred. With PSR.ic = 0 every deferrable fault defers (always-defer
 * model, Vol.2 §5.5.5 Table 5-2), so a TLB miss gives NaT instead of a
 * fault; a hit uses the real access path, including any translation
 * cache the implementation keeps behind the architectural TC. PSR.dt is
 * restored to the caller's value: APs run here in physical mode, the BSP
 * in whatever mode the firmware runs boot services. */
    .align 32
    .global smp_probe#
    .type smp_probe#, @function
    .proc smp_probe#
smp_probe:
    mov r16 = psr
    ;;
    rsm 0x6000
    ;;
    srlz.d
    ;;
    ssm 0x20000                 /* psr.dt */
    ;;
    srlz.d
    ;;
    ld8.s r17 = [r32]
    tbit.z p8, p0 = r16, 17     /* the caller ran with psr.dt = 0 */
    ;;
(p8) rsm 0x20000
    ;;
    srlz.d
    tbit.nz p6, p0 = r16, 13
    tbit.nz p7, p0 = r16, 14
    ;;
(p6) ssm 0x2000
    ;;
(p7) ssm 0x4000
    ;;
    srlz.d
    tnat.nz p8, p9 = r17
    ;;
(p8) mov r8 = 0
(p9) mov r8 = 1
(p9) st8 [r33] = r17
    br.ret.sptk.many b0
    ;;
    .endp smp_probe#

/* UINT64 smp_set_rr(va, value): writes the region register of va's
 * region, returns the old value (Vol.2 §4.1.2; srlz.d before data use). */
    .align 32
    .global smp_set_rr#
    .type smp_set_rr#, @function
    .proc smp_set_rr#
smp_set_rr:
    mov r8 = rr[r32]
    ;;
    mov rr[r32] = r33
    ;;
    srlz.d
    br.ret.sptk.many b0
    ;;
    .endp smp_set_rr#
