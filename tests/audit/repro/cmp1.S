/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT p6 = 0
// EXPECT p7 = 0
// EXPECT p8 = 0
// EXPECT p9 = 0
// EXPECT p11 = 1
// EXPECT p12 = 1
// EXPECT p13 = 0
// EXPECT p14 = 0
// EXPECT p15 = 0
// EXPECT p16 = 0
// EXPECT p17 = 0
// EXPECT p18 = 0
// EXPECT p19 = 1
// EXPECT p20 = 1
// EXPECT p21 = 1
// EXPECT p22 = 0
// EXPECT p23 = 0
// EXPECT p24 = 1
// EXPECT p25 = 0
// EXPECT p26 = 0
// EXPECT p27 = 1
// EXPECT p28 = 0
// EXPECT p29 = 0
// EXPECT p30 = 0
// EXPECT p31 = 1
// EXPECT p32 = 1
// EXPECT p33 = 0
// EXPECT p34 = 1
// EXPECT p35 = 1
// EXPECT p36 = 0
// EXPECT p37 = 0
// EXPECT p38 = 0
// EXPECT p39 = 1
// EXPECT p40 = 1
// EXPECT p41 = 0
// EXPECT p42 = 1
// EXPECT p43 = 0
// EXPECT p44 = 0
// EXPECT p45 = 0
// EXPECT p46 = 1
// EXPECT p47 = 1
// EXPECT p48 = 1
// EXPECT p49 = 0
// EXPECT p50 = 0
// EXPECT p51 = 0
// EXPECT p52 = 0
// EXPECT p53 = 0
// EXPECT p54 = 0
// EXPECT nat40 = 1

.text
.global _start
_start:
  movl r2 = 0x108000
  mov r3 = 1
  movl r4 = 0x7f9c7400067b00
  ;;
  mov ar.unat = r3
  mov pr = r4, -1
  ;;
  ld8.fill r40 = [r2]
  mov r9 = -1
  movl r20 = 0xffffffff00000000
  movl r21 = 0x00000000ffffffff
  movl r22 = 0x100000005
  mov r23 = 5
  ;;
  cmp.eq p6,p7 = r40, r0
  (p10) cmp.eq.unc p8,p9 = r0, r0
  (p10) cmp.eq.and p11,p12 = r0, r0
  cmp.eq.and p13,p14 = r40, r40
  cmp.eq.or p15,p16 = r40, r40
  cmp.lt.and p17,p18 = r0, r9
  cmp.gt.or p19,p20 = r0, r9
  cmp4.lt p21,p22 = -1, r20
  cmp4.ltu p23,p24 = r21, r20
  tbit.z p25,p26 = r40, 0
  tnat.nz p27,p28 = r40
  tnat.z.and p29,p30 = r40
  tbit.nz.or p31,p32 = r9, 63
  tbit.z.or.andcm p33,p34 = r9, 5
  tbit.nz.or.andcm p35,p36 = r9, 5
  tnat.nz.and p37,p38 = r0
  tnat.z.or p39,p40 = r0
  cmp.ne.or.andcm p41,p42 = r9, r40
  cmp.eq p43,p44 = 5, r40
  cmp.ltu p45,p46 = -1, r9
  cmp4.eq.and p47,p48 = r22, r23
  (p10) tbit.z.unc p49,p50 = r9, 0
  (p10) tnat.z.unc p51,p52 = r0
  cmp.lt.unc p53,p54 = r40, r0
  ;;
  break.i 0x1ffffe

