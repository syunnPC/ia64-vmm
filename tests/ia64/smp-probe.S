/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Regression for the pre-purge probe, copied from tests/efi/smp/entry.S. */
// EXPECT r8 = 1
// EXPECT mem[0x109008] = 0x5350505552474531
.text
.global _start
_start:
    alloc r31=ar.pfs,0,0,2,0
    movl r20=0x109000
    movl r21=0x5350505552474531
    st8 [r20]=r21
    movl r22=0x2000000005000000
    movl r23=0x3fffc34
    mov rr[r22]=r23
    mov cr.ifa=r22
    mov r24=13<<2
    mov cr.itir=r24
    movl r25=0x108661
    itc.d r25
    ;;
    srlz.d
    adds r32=4096,r22
    adds r33=8,r20
    br.call.sptk.many b0=smp_probe
    ;;
    break.i 0x1ffffe
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

