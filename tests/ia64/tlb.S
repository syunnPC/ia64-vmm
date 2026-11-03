/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x300123
// EXPECT r9 = 0x300
// EXPECT r10 = 1
// EXPECT r11 = 0
// EXPECT r12 = 0x802000
// EXPECT r13 = 0x808080
// EXPECT r14 = 0x20000000400
// EXPECT r15 = 1
// EXPECT nat24 = 1
.text
.global _start
_start:
    rsm psr.ic
    movl r20=0x400123
    movl r21=0x300781
    mov r22=0x430
    ;;
    mov rr[r20]=r22
    mov cr.ifa=r20
    mov r22=0x330
    ;;
    mov cr.itir=r22
    mov r23=0
    ;;
    itr.d dtr[r23]=r21
    srlz.d
    ;;
    tpa r8=r20
    tak r9=r20
    probe.r r10=r20,3
    mov r22=0x305
    ;;
    mov pkr[r23]=r22
    ssm psr.pk
    ;;
    probe.r r11=r20,0
    mov r22=0x301
    ;;
    mov pkr[r23]=r22
    ;;
    probe.r r15=r20,0
    movl r22=0x800040
    ;;
    mov cr.pta=r22
    ;;
    thash r12=r20
    ttag r14=r20
    movl r22=0x800140
    ;;
    mov cr.pta=r22
    ;;
    thash r13=r20
    movl r22=0x8000000000000
    ;;
    ld8.s r24=[r22]
    ;;
    thash r24=r24
    rsm psr.pk
    mov r22=48
    ;;
    ptr.d r20,r22
    ptc.e r20
    break.i 0x1ffffe
