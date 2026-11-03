/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT p6 = 1
// EXPECT p7 = 1
// EXPECT p8 = 0
// EXPECT p9 = 0
// EXPECT p10 = 1
// EXPECT p11 = 0
// EXPECT p12 = 0
// EXPECT p13 = 0
// EXPECT p14 = 0
// EXPECT p15 = 1
.text
.global _start
_start:
    mov r8=5
    mov r9=6
    mov r10=-1
    cmp.eq.or p6,p7=r8,r8
    cmp.eq.and p8,p9=r8,r9
    cmp.eq.or.andcm p10,p11=r8,r8
    cmp.eq.orcm p12,p13=r8,r9
    cmp.eq.andcm p12,p13=r8,r8
    cmp.eq.and.orcm p14,p15=r8,r9
    break.i 0x1ffffe
