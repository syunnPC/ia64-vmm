/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1   add R = r2, r3
// EXPECT r9 = 0x2   add R = r2, r3, 1
// EXPECT r10 = 0x1   sub R = r2, r3
// EXPECT r11 = 0x0   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffeb   sub R = -21, r3
// EXPECT r13 = 0x0   and R = -21, r3
// EXPECT r14 = 0xffffffffffffffeb   or R = -21, r3
// EXPECT r15 = 0xffffffffffffffeb   xor R = -21, r3
// EXPECT r16 = 0xffffffffffffffeb   andcm R = -21, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x1   or R = r2, r3
// EXPECT r19 = 0x1   xor R = r2, r3
// EXPECT r20 = 0x1   andcm R = r2, r3
// EXPECT r21 = 0x12c1   adds R = 4801, r3
// EXPECT r22 = 0x3fdd9   addl R = 261593, r1
// EXPECT r23 = 0x2   shladd R = r2, 1, r3
// EXPECT r24 = 0x2   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x4   shladd R = r2, 2, r3
// EXPECT r26 = 0x4   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x8   shladd R = r2, 3, r3
// EXPECT r28 = 0x8   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x10   shladd R = r2, 4, r3
// EXPECT r30 = 0x10   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x1   addp4 R = r2, r3
// EXPECT r32 = 0x12c1   addp4 R = 4801, r3
// EXPECT r33 = 0x8000000000000000   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 43, 6
// EXPECT r37 = 0x0   extr R = r3, 43, 6
// EXPECT r38 = 0x80000000000   dep.z R = r2, 43, 6
// EXPECT r39 = 0x1300000000000   dep.z R = -26, 43, 6
// EXPECT r40 = 0x80000000000   dep R = r2, r3, 43, 1
// EXPECT r41 = 0x1f80000000000   dep R = -1, r3, 43, 6
// EXPECT r42 = 0x200000000000000   shrp R = r2, r3, 7
// EXPECT r43 = 0x80   shl R = r2, 7
// EXPECT r44 = 0x0   shr.u R = r3, 7
// EXPECT r45 = 0x0   shr R = r3, 7
// EXPECT r46 = 0x0   extr.u R = r3, 48, 63
// EXPECT r47 = 0x0   extr R = r3, 48, 63
// EXPECT r48 = 0x1000000000000   dep.z R = r2, 48, 63
// EXPECT r49 = 0xffc7000000000000   dep.z R = -57, 48, 63
// EXPECT r50 = 0x1000000000000   dep R = r2, r3, 48, 1
// EXPECT r51 = 0x0   dep R = 0, r3, 48, 63
// EXPECT r52 = 0x400   shrp R = r2, r3, 54
// EXPECT r53 = 0x40000000000000   shl R = r2, 54
// EXPECT r54 = 0x0   shr.u R = r3, 54
// EXPECT r55 = 0x0   shr R = r3, 54
// EXPECT r56 = 0x0   extr.u R = r3, 14, 1
// EXPECT r57 = 0x0   extr R = r3, 14, 1
// EXPECT r58 = 0x4000   dep.z R = r2, 14, 1
// EXPECT r59 = 0x0   dep.z R = -66, 14, 1
// EXPECT r60 = 0x4000   dep R = r2, r3, 14, 7
// EXPECT r61 = 0x0   dep R = 0, r3, 14, 1
// EXPECT r62 = 0x400000   shrp R = r2, r3, 42
// EXPECT r63 = 0x40000000000   shl R = r2, 42
// EXPECT r64 = 0x0   shr.u R = r3, 42
// EXPECT r65 = 0x0   shr R = r3, 42
// EXPECT r66 = 0x0   extr.u R = r3, 1, 11
// EXPECT r67 = 0x0   extr R = r3, 1, 11
// EXPECT r68 = 0x2   dep.z R = r2, 1, 11
// EXPECT r69 = 0xf88   dep.z R = -60, 1, 11
// EXPECT r70 = 0x2   dep R = r2, r3, 1, 1
// EXPECT r71 = 0x0   dep R = 0, r3, 1, 11
// EXPECT r72 = 0x2   shrp R = r2, r3, 63
// EXPECT r73 = 0x8000000000000000   shl R = r2, 63
// EXPECT r74 = 0x0   shr.u R = r3, 63
// EXPECT r75 = 0x0   shr R = r3, 63
// EXPECT r76 = 0x0   extr.u R = r3, 25, 54
// EXPECT r77 = 0x0   extr R = r3, 25, 54
// EXPECT r78 = 0x2000000   dep.z R = r2, 25, 54
// EXPECT r79 = 0xffffffff46000000   dep.z R = -93, 25, 54
// EXPECT r80 = 0x2000000   dep R = r2, r3, 25, 13
// EXPECT r81 = 0x0   dep R = 0, r3, 25, 54
// EXPECT r82 = 0x20000000000000   shrp R = r2, r3, 11
// EXPECT r83 = 0x800   shl R = r2, 11
// EXPECT r84 = 0x0   shr.u R = r3, 11
// EXPECT r85 = 0x0   shr R = r3, 11
// EXPECT r86 = 0x0   extr.u R = r3, 18, 23
// EXPECT r87 = 0x0   extr R = r3, 18, 23
// EXPECT r88 = 0x40000   dep.z R = r2, 18, 23
// EXPECT r89 = 0x1fffe540000   dep.z R = -107, 18, 23
// EXPECT r90 = 0x40000   dep R = r2, r3, 18, 2
// EXPECT r91 = 0x1fffffc0000   dep R = -1, r3, 18, 23
// EXPECT r92 = 0x200000000000   shrp R = r2, r3, 19
// EXPECT r93 = 0x80000   shl R = r2, 19
// EXPECT r94 = 0x0   shr.u R = r3, 19
// EXPECT r95 = 0x0   shr R = r3, 19
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x0   popcnt R = r3
// EXPECT p6 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp.ge P1, P2 = -84, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp.lt P1, P2 = -66, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp.geu P1, P2 = 49, r3
// EXPECT p15 = 0
// EXPECT p16 = 0   tbit.nz P1, P2 = r3, 55
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp4.gt P1, P2 = -119, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.ne P1, P2 = -111, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp.eq P1, P2 = 9, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp4.gtu P1, P2 = 66, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp4.ltu P1, P2 = -21, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp4.leu P1, P2 = -27, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p37 = 0
// EXPECT p38 = 1   cmp.gtu P1, P2 = 59, r3
// EXPECT p39 = 0
// EXPECT p40 = 1   tbit.z P1, P2 = r3, 55
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp4.ne P1, P2 = 59, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.gt P1, P2 = -106, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   tbit.z P1, P2 = r3, 27
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp.ltu P1, P2 = 78, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.leu P1, P2 = -46, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   tbit.nz P1, P2 = r3, 28
// EXPECT p61 = 1
// EXPECT p62 = 0   cmp4.eq P1, P2 = -72, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x1
  movl r3 = 0x0
  movl r4 = 0x3f
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -21, r3
  and r13 = -21, r3
  or r14 = -21, r3
  xor r15 = -21, r3
  andcm r16 = -21, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 4801, r3
  addl r22 = 261593, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 4801, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 43, 6
  extr r37 = r3, 43, 6
  dep.z r38 = r2, 43, 6
  dep.z r39 = -26, 43, 6
  dep r40 = r2, r3, 43, 1
  dep r41 = -1, r3, 43, 6
  shrp r42 = r2, r3, 7
  shl r43 = r2, 7
  shr.u r44 = r3, 7
  shr r45 = r3, 7
  extr.u r46 = r3, 48, 63
  extr r47 = r3, 48, 63
  dep.z r48 = r2, 48, 63
  dep.z r49 = -57, 48, 63
  dep r50 = r2, r3, 48, 1
  dep r51 = 0, r3, 48, 63
  shrp r52 = r2, r3, 54
  shl r53 = r2, 54
  shr.u r54 = r3, 54
  shr r55 = r3, 54
  extr.u r56 = r3, 14, 1
  extr r57 = r3, 14, 1
  dep.z r58 = r2, 14, 1
  dep.z r59 = -66, 14, 1
  dep r60 = r2, r3, 14, 7
  dep r61 = 0, r3, 14, 1
  shrp r62 = r2, r3, 42
  shl r63 = r2, 42
  shr.u r64 = r3, 42
  shr r65 = r3, 42
  extr.u r66 = r3, 1, 11
  extr r67 = r3, 1, 11
  dep.z r68 = r2, 1, 11
  dep.z r69 = -60, 1, 11
  dep r70 = r2, r3, 1, 1
  dep r71 = 0, r3, 1, 11
  shrp r72 = r2, r3, 63
  shl r73 = r2, 63
  shr.u r74 = r3, 63
  shr r75 = r3, 63
  extr.u r76 = r3, 25, 54
  extr r77 = r3, 25, 54
  dep.z r78 = r2, 25, 54
  dep.z r79 = -93, 25, 54
  dep r80 = r2, r3, 25, 13
  dep r81 = 0, r3, 25, 54
  shrp r82 = r2, r3, 11
  shl r83 = r2, 11
  shr.u r84 = r3, 11
  shr r85 = r3, 11
  extr.u r86 = r3, 18, 23
  extr r87 = r3, 18, 23
  dep.z r88 = r2, 18, 23
  dep.z r89 = -107, 18, 23
  dep r90 = r2, r3, 18, 2
  dep r91 = -1, r3, 18, 23
  shrp r92 = r2, r3, 19
  shl r93 = r2, 19
  shr.u r94 = r3, 19
  shr r95 = r3, 19
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.le p6, p7 = r2, r3
  cmp.ge p8, p9 = -84, r3
  cmp4.eq p10, p11 = r2, r3
  cmp.lt p12, p13 = -66, r3
  cmp.geu p14, p15 = 49, r3
  tbit.nz p16, p17 = r3, 55
  cmp.geu p18, p19 = r2, r3
  cmp.ltu p20, p21 = r2, r3
  cmp4.gt p22, p23 = -119, r3
  cmp.ne p24, p25 = -111, r3
  cmp.eq p26, p27 = 9, r3
  cmp4.lt p28, p29 = r2, r3
  cmp4.gtu p30, p31 = 66, r3
  cmp4.ltu p32, p33 = -21, r3
  cmp4.leu p34, p35 = -27, r3
  cmp4.ne p36, p37 = r2, r3
  cmp.gtu p38, p39 = 59, r3
  tbit.z p40, p41 = r3, 55
  cmp4.ne p42, p43 = 59, r3
  cmp.ne p44, p45 = r2, r3
  cmp.gt p46, p47 = -106, r3
  tbit.z p48, p49 = r3, 27
  cmp.ltu p50, p51 = 78, r3
  cmp4.geu p52, p53 = r2, r3
  cmp.gt p54, p55 = r2, r3
  cmp.ge p56, p57 = r2, r3
  cmp.leu p58, p59 = -46, r3
  tbit.nz p60, p61 = r3, 28
  cmp4.eq p62, p63 = -72, r3
  ;;
  break.i 0x1ffffe
