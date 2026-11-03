/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1122334455667788
// EXPECT r9 = 0x99aabbccddeeff00
// EXPECT r10 = 0x99aabbccddeeff00
// EXPECT r11 = 0x77
// EXPECT r12 = 0x88
// EXPECT r13 = 0x20000704
// EXPECT r14 = 0x5
// EXPECT r15 = 0x111
// EXPECT r16 = 0x222
// EXPECT nat17 = 1
// EXPECT nat18 = 1
// EXPECT p6 = 1
// EXPECT p7 = 0
.text
.global _start
_start:
    movl r20=0x108000
    movl r21=0x1122334455667788
    movl r22=0x99aabbccddeeff00
    ;;
    mov ar.csd=r22
    ;;
    st16.rel [r20]=r21,ar.csd
    ;;
    ld16.acq r8,ar.csd=[r20]
    mov r9=ar.csd
    mov ar.ccv=r22
    ;;
    adds r23=8,r20
    mov r21=0x77
    mov r22=0x88
    mov ar.csd=r22
    ;;
    cmp8xchg16.acq r10=[r23],r21,ar.csd,ar.ccv
    ;;
    ld16 r11,ar.csd=[r20]
    mov r12=ar.csd
    mov r23=3
    ;;
    mov r13=cpuid[r23]
    mov r23=4
    ;;
    mov r14=cpuid[r23]
    bsw.1
    ;;
    mov r16=0x111
    bsw.0
    ;;
    mov r16=0x222
    bsw.1
    ;;
    mov r15=r16
    bsw.0
    ;;
    movl r20=0x8000000000000
    ;;
    ld8.s r17=[r20]
    movl r20=0x108000
    ;;
    ld8.sa r18=[r20]
    lfetch [r20],8
    fc r20
    fc.i r20
    sync.i
    ;;
    srlz.d
    srlz.i
    tf.z p6,p7=32
    break.i 0x1ffffe
