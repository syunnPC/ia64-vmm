/* SPDX-License-Identifier: GPL-2.0-or-later */
// Big-endian semaphores and 16-byte accesses with UM.be = 1. Byte order
// applies to the whole semaphore datum (SDM Vol.3 Table 3-1 mem_xchg,
// mem_xchg_add, mem_xchg_cond); ld16/st16/cmp8xchg16 order bytes within
// each 8-byte half, GR at the lower address (mem_read_pair, mem_write16,
// mem_xchg16_cond). cmpxchg compares the zero-extended datum with all of
// AR.CCV; fetchadd wraps within the datum (r25-r30: 0xffffffff + 1, 0x100 - 16,
// a 33-bit AR.CCV). Memory is set up and read back with UM.be = 0.
// EXPECT r8 = 0x1
// EXPECT r9 = 0x2
// EXPECT r10 = 0x1122334455667788
// EXPECT r11 = 0xa1a2a3a4a5a6a7a8
// EXPECT r12 = 0xff000000
// EXPECT r13 = 0x1234
// EXPECT r14 = 0x1234
// EXPECT r15 = 0xabcd
// EXPECT r16 = 0x0102030405060708
// EXPECT r17 = 0x1112131415161718
// EXPECT r18 = 0x1112131415161718
// EXPECT r19 = 0xa1a2a3a4a5a6a7a8
// EXPECT r20 = 0xa8a7a6a5a4a3a2a1
// EXPECT r21 = 0x2827262524232221
// EXPECT r22 = 0xa8a7a6a5a4a3a2a1
// EXPECT r23 = 0x010000ff
// EXPECT r24 = 0x5713
// EXPECT r25 = 0xffffffff
// EXPECT r26 = 0x0
// EXPECT r27 = 0x100
// EXPECT r28 = 0xf000000000000000
// EXPECT r29 = 0x12345678
// EXPECT r30 = 0x78563412
.text
.global _start
_start:
    movl r2=0x10a000
    movl r3=0x0100000000000000
    ;;
    adds r4=0x30,r2
    adds r5=0x38,r2
    adds r6=0x3c,r2
    ;;
    st8 [r4]=r3
    mov r3=0xff
    ;;
    st4 [r5]=r3
    mov r3=0x3412
    ;;
    st2 [r6]=r3
    adds r4=0x50,r2
    adds r5=0x58,r2
    adds r6=0x60,r2
    mov r3=-1
    ;;
    st4 [r4]=r3
    movl r3=0x0001000000000000
    ;;
    st8 [r5]=r3
    movl r3=0x78563412
    ;;
    st4 [r6]=r3
    sum 0x2
    ;;
    fetchadd4.acq r25=[r4],1
    fetchadd8.rel r27=[r5],-16
    movl r3=0x112345678
    ;;
    mov ar.ccv=r3
    ;;
    cmpxchg4.acq r29=[r6],r0,ar.ccv
    adds r4=0x30,r2
    adds r5=0x38,r2
    adds r6=0x3c,r2
    ;;
    fetchadd8.acq r8=[r4],1
    ;;
    movl r3=0x1122334455667788
    ;;
    xchg8 r9=[r4],r3
    mov ar.ccv=r3
    movl r7=0xa1a2a3a4a5a6a7a8
    ;;
    cmpxchg8.acq r10=[r4],r7,ar.ccv
    ;;
    cmpxchg8.rel r11=[r4],r3,ar.ccv
    fetchadd4.rel r12=[r5],1
    movl r3=0x11234
    ;;
    mov ar.ccv=r3
    mov r7=0xabcd
    ;;
    cmpxchg2.acq r13=[r6],r7,ar.ccv
    mov r3=0x1234
    ;;
    mov ar.ccv=r3
    ;;
    cmpxchg2.acq r14=[r6],r7,ar.ccv
    mov r3=0x1357
    ;;
    xchg2 r15=[r6],r3
    adds r4=0x40,r2
    movl r3=0x0102030405060708
    movl r7=0x1112131415161718
    ;;
    mov ar.csd=r7
    ;;
    st16 [r4]=r3,ar.csd
    ;;
    mov ar.csd=r0
    ;;
    ld16 r16,ar.csd=[r4]
    ;;
    mov r17=ar.csd
    adds r5=8,r4
    mov ar.ccv=r7
    movl r3=0xa1a2a3a4a5a6a7a8
    movl r6=0x2122232425262728
    ;;
    mov ar.csd=r6
    ;;
    cmp8xchg16.acq r18=[r5],r3,ar.csd,ar.ccv
    ;;
    cmp8xchg16.rel r19=[r4],r3,ar.csd,ar.ccv
    rum 0x2
    ;;
    ld8 r20=[r4]
    ld8 r21=[r5]
    adds r4=0x30,r2
    adds r5=0x38,r2
    adds r6=0x3c,r2
    ;;
    ld8 r22=[r4]
    ld4 r23=[r5]
    ld2 r24=[r6]
    adds r4=0x50,r2
    adds r5=0x58,r2
    adds r6=0x60,r2
    ;;
    ld4 r26=[r4]
    ld8 r28=[r5]
    ld4 r30=[r6]
    break.i 0x1ffffe
