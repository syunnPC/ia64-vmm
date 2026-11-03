/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT p6 = 0   cmp.eq.unc P = r8, r9
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp.ne.unc P = r8, r9
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.lt.unc P = r8, r9
// EXPECT p11 = 0
// EXPECT p12 = 0   cmp.le.unc P = r8, r9
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.gt.unc P = r8, r9
// EXPECT p15 = 0
// EXPECT p16 = 0   cmp.ge.unc P = r8, r9
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.ltu.unc P = r8, r9
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.leu.unc P = r8, r9
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.gtu.unc P = r8, r9
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.geu.unc P = r8, r9
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.eq.unc P = r8, r9
// EXPECT p27 = 0
// EXPECT p28 = 0   cmp4.lt.unc P = r8, r9
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.ltu.unc P = r8, r9
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp4.ge.unc P = r8, r9
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp4.geu.unc P = r8, r9
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp.eq.unc P = 3, r9
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.lt.unc P = 3, r9
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.ltu.unc P = 3, r9
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp.gt.unc P = 3, r9
// EXPECT p43 = 0
// EXPECT p44 = 0   cmp4.eq.unc P = 3, r9
// EXPECT p45 = 0
.text
.global _start
_start:
  movl r4 = 0x3fffffffffc0
  mov r8 = 1
  mov r9 = 2
  ;;
  mov pr = r4, -1
  ;;
  (p5) cmp.eq.unc p6, p7 = r8, r9
  (p5) cmp.ne.unc p8, p9 = r8, r9
  (p5) cmp.lt.unc p10, p11 = r8, r9
  (p5) cmp.le.unc p12, p13 = r8, r9
  (p5) cmp.gt.unc p14, p15 = r8, r9
  (p5) cmp.ge.unc p16, p17 = r8, r9
  (p5) cmp.ltu.unc p18, p19 = r8, r9
  (p5) cmp.leu.unc p20, p21 = r8, r9
  (p5) cmp.gtu.unc p22, p23 = r8, r9
  (p5) cmp.geu.unc p24, p25 = r8, r9
  (p5) cmp4.eq.unc p26, p27 = r8, r9
  (p5) cmp4.lt.unc p28, p29 = r8, r9
  (p5) cmp4.ltu.unc p30, p31 = r8, r9
  (p5) cmp4.ge.unc p32, p33 = r8, r9
  (p5) cmp4.geu.unc p34, p35 = r8, r9
  (p5) cmp.eq.unc p36, p37 = 3, r9
  (p5) cmp.lt.unc p38, p39 = 3, r9
  (p5) cmp.ltu.unc p40, p41 = 3, r9
  (p5) cmp.gt.unc p42, p43 = 3, r9
  (p5) cmp4.eq.unc p44, p45 = 3, r9
  ;;
  break.i 0x1ffffe
