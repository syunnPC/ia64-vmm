/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x0000000000000000
// EXPECT r9 = 0x0001020304050607
// EXPECT r10 = 0x0000000cfffffff6
// EXPECT r11 = 0x55667788ddeeff00
.text
.global _start
_start:
    mov r20=-1
    movl r21=0x0101010101010101
    movl r22=0x0706050403020100
    movl r23=0x000000030000fffe
    movl r24=0x0000000400000005
    movl r25=0x1122334455667788
    movl r26=0x99aabbccddeeff00
    padd1 r8=r20,r21
    mux1 r9=r22,@rev
    pmpy2.r r10=r23,r24
    unpack4.l r11=r25,r26
    break.i 0x1ffffe
