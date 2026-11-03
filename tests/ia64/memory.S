/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1122334455667788
// EXPECT r9 = 0x0000000055667788
// EXPECT r10 = 0x1122334455667788
// EXPECT r11 = 0x00000000000000aa
// EXPECT r12 = 0x00000000000000bb
// EXPECT r13 = 0x00000000000000cc
// EXPECT r14 = 0x00000000000000cc
// EXPECT r15 = 0x0000000000000001
// EXPECT r16 = 0x0000000000000001
// EXPECT r17 = 0x0000000000000001
// EXPECT mem[0x108000] = 0x00000000000000bc
.text
.global _start
_start:
    movl r20=0x108000
    movl r21=0x1122334455667788
    st8.rel [r20]=r21
    ld8.acq r8=[r20],8
    st4 [r20]=r21,8
    adds r22=-8,r20
    ld4 r9=[r22]
    mov ar.unat=r0
    st8.spill [r20]=r21
    ld8.fill r10=[r20]
    mov r23=0xaa
    adds r24=-16,r20
    st8 [r24]=r23
    mov r25=0xbb
    xchg8 r11=[r24],r25
    mov ar.ccv=r25
    mov r26=0xcc
    cmpxchg8.acq r12=[r24],r26,ar.ccv
    cmpxchg8.rel r13=[r24],r25,ar.ccv
    xchg8 r14=[r24],r25
    fetchadd8.acq r18=[r24],1
    ld8.a r19=[r24]
    chk.a.clr r19,.recovery
    mov r15=0
    br .done
.recovery:
    mov r15=1
.done:
    tnat.z p6,p7=r10
    (p6) mov r16=1
    (p7) mov r16=0
    ld8.c.clr r19=[r24]
    mov r27=0xbc
    cmp.eq p6,p7=r27,r19
    (p6) mov r17=1
    (p7) mov r17=0
    mf
    break.i 0x1ffffe
