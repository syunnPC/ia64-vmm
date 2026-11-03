/* SPDX-License-Identifier: GPL-2.0-or-later */
// Big-endian integer loads and stores with UM.be = 1 (SDM Vol.1 §3.2.3,
// Figure 3-14): bytes at lower addresses are the more significant ones.
// Memory is written and read back with UM.be = 0 around the accesses.
// EXPECT r8 = 0x2
// EXPECT r9 = 0x0304
// EXPECT r10 = 0x05060708
// EXPECT r11 = 0x0102030405060708
// EXPECT r12 = 0x6655443300002211
// EXPECT r13 = 0x0807060504030201
// EXPECT r14 = 0x1122334455667788
// EXPECT r15 = 0x8877665544332211
// EXPECT r16 = 0x0102030405060708
// EXPECT r17 = 0x01020304
// EXPECT r18 = 0x0304
// EXPECT r19 = 0x0102030405060708
// EXPECT r20 = 0x3
// EXPECT nat16 = 0
// EXPECT nat14 = 0
.text
.global _start
_start:
    movl r2=0x10a000
    movl r3=0x0807060504030201
    ;;
    st8 [r2]=r3
    adds r4=0x10,r2
    ;;
    st8 [r4]=r0,8
    ;;
    st8 [r4]=r0
    sum 0x2
    ;;
    adds r4=1,r2
    adds r5=2,r2
    adds r6=4,r2
    ;;
    ld1 r8=[r4]
    ld2 r9=[r5]
    ld4.acq r10=[r6]
    ld8 r11=[r2]
    adds r4=0x10,r2
    adds r5=0x14,r2
    adds r6=0x18,r2
    mov r3=0x1122
    movl r7=0x33445566
    ;;
    st2 [r4]=r3
    st4 [r5]=r7
    movl r3=0x0102030405060708
    ;;
    st8.rel [r6]=r3
    adds r4=0x20,r2
    movl r3=0x1122334455667788
    mov r7=0x13
    ;;
    mov ar.unat=r7
    ;;
    st8.spill [r4]=r3
    ;;
    ld8.fill r14=[r4]
    ld8.c.nc r16=[r2]
    ld4.a r17=[r6]
    adds r5=0x1a,r2
    ;;
    ld2.c.clr r18=[r5]
    ld8.bias r19=[r6]
    mov r20=ar.unat
    rum 0x2
    ;;
    adds r4=0x10,r2
    adds r5=0x18,r2
    adds r6=0x20,r2
    ;;
    ld8 r12=[r4]
    ld8 r13=[r5]
    ld8 r15=[r6]
    break.i 0x1ffffe
