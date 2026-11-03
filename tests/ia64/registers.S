/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x0000000000000123
// EXPECT r9 = 0x0000000000000456
// EXPECT r10 = 0x0000000000000123
// EXPECT r11 = 0x0000000000000001
// EXPECT p6 = 1
// EXPECT p7 = 0
.text
.global _start
_start:
    mov r20=0x123
    mov r21=0x456
    mov ar.ccv=r20
    mov ar.unat=r21
    mov b3=r20
    mov r8=ar.ccv
    mov r9=ar.unat
    mov r10=b3
    cmp.eq p6,p7=r8,r10
    mov r22=pr
    tbit.nz p8,p9=r22,6
    (p8) mov r11=1
    (p9) mov r11=0
    break.i 0x1ffffe
