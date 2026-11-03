/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT p6 = 0   cmp4.lt.unc P = 3, r9
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp4.ltu.unc P = 3, r9
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.geu.unc P = 3, r9
// EXPECT p11 = 0
// EXPECT p12 = 0   tbit.z.unc P = r9, 3
// EXPECT p13 = 0
// EXPECT p14 = 0   tbit.nz.unc P = r9, 3
// EXPECT p15 = 0
// EXPECT p16 = 0   tnat.z.unc P = r9
// EXPECT p17 = 0
// EXPECT p18 = 0   tnat.nz.unc P = r9
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp.ne.and P = r8, r9
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp.ne.or P = r8, r9
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.ne.or.andcm P = r8, r9
// EXPECT p25 = 1
// EXPECT p26 = 1   cmp4.ne.and P = r8, r9
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.ne.and P = 3, r9
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp4.ne.or P = 3, r9
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.lt.and P = r0, r9
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp.le.or P = r0, r9
// EXPECT p35 = 1
// EXPECT p36 = 1   tbit.nz.and P = r9, 3
// EXPECT p37 = 1
// EXPECT p38 = 1   tnat.nz.or P = r9
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp.ne.andcm P = r8, r9
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp.eq.orcm P = r8, r9
// EXPECT p43 = 1
.text
.global _start
_start:
  movl r4 = 0xfffffffffc0
  mov r8 = 1
  mov r9 = 2
  ;;
  mov pr = r4, -1
  ;;
  (p5) cmp4.lt.unc p6, p7 = 3, r9
  (p5) cmp4.ltu.unc p8, p9 = 3, r9
  (p5) cmp4.geu.unc p10, p11 = 3, r9
  (p5) tbit.z.unc p12, p13 = r9, 3
  (p5) tbit.nz.unc p14, p15 = r9, 3
  (p5) tnat.z.unc p16, p17 = r9
  (p5) tnat.nz.unc p18, p19 = r9
  (p5) cmp.ne.and p20, p21 = r8, r9
  (p5) cmp.ne.or p22, p23 = r8, r9
  (p5) cmp.ne.or.andcm p24, p25 = r8, r9
  (p5) cmp4.ne.and p26, p27 = r8, r9
  (p5) cmp.ne.and p28, p29 = 3, r9
  (p5) cmp4.ne.or p30, p31 = 3, r9
  (p5) cmp.lt.and p32, p33 = r0, r9
  (p5) cmp.le.or p34, p35 = r0, r9
  (p5) tbit.nz.and p36, p37 = r9, 3
  (p5) tnat.nz.or p38, p39 = r9
  (p5) cmp.ne.andcm p40, p41 = r8, r9
  (p5) cmp.eq.orcm p42, p43 = r8, r9
  ;;
  break.i 0x1ffffe
