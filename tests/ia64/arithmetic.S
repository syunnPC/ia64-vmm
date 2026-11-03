/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x8000000000000000
// EXPECT r9 = 0xffffffffffffffff
// EXPECT r10 = 0x4000000080000001
// EXPECT r11 = 0x0000000000001234
// EXPECT r14 = 0xffffffffffffff80
// EXPECT r15 = 0x000000000000ffff
// EXPECT r16 = 0x0000000000000040
// EXPECT r17 = 0x0000000000000002
// EXPECT r18 = 0x8000000000000000
// EXPECT p6 = 1
// EXPECT p7 = 0
.text
.global _start
_start:
    mov r20=1
    mov r21=63
    movl r22=0x80000000
    shl r8=r20,r21
    sub r9=r0,r20
    addp4 r10=r20,r22
    movl r11=0x1234
    mov r23=0x80
    sxt1 r14=r23
    zxt2 r15=r9
    popcnt r16=r9
    shrp r17=r20,r0,63
    dep.z r18=r20,63,1
    cmp.ne p6,p7=r14,r23
    break.i 0x1ffffe
