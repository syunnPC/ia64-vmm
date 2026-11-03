/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x8000000000000002   add R = r2, r3
// EXPECT r9 = 0x8000000000000003   add R = r2, r3, 1
// EXPECT r10 = 0x7ffffffffffffffe   sub R = r2, r3
// EXPECT r11 = 0x7ffffffffffffffd   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffff7e   sub R = -128, r3
// EXPECT r13 = 0x0   and R = -128, r3
// EXPECT r14 = 0xffffffffffffff82   or R = -128, r3
// EXPECT r15 = 0xffffffffffffff82   xor R = -128, r3
// EXPECT r16 = 0xffffffffffffff80   andcm R = -128, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x8000000000000002   or R = r2, r3
// EXPECT r19 = 0x8000000000000002   xor R = r2, r3
// EXPECT r20 = 0x8000000000000000   andcm R = r2, r3
// EXPECT r21 = 0xffffffffffffe0b0   adds R = -8018, r3
// EXPECT r22 = 0xfffffffffffacf77   addl R = -340105, r1
// EXPECT r23 = 0x2   shladd R = r2, 1, r3
// EXPECT r24 = 0x2   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x2   shladd R = r2, 2, r3
// EXPECT r26 = 0x2   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x2   shladd R = r2, 3, r3
// EXPECT r28 = 0x2   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x2   shladd R = r2, 4, r3
// EXPECT r30 = 0x2   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x2   addp4 R = r2, r3
// EXPECT r32 = 0xffffe0b0   addp4 R = -8018, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 27, 22
// EXPECT r37 = 0x0   extr R = r3, 27, 22
// EXPECT r38 = 0x0   dep.z R = r2, 27, 22
// EXPECT r39 = 0x1fffea8000000   dep.z R = -43, 27, 22
// EXPECT r40 = 0x2   dep R = r2, r3, 27, 10
// EXPECT r41 = 0x1fffff8000002   dep R = -1, r3, 27, 22
// EXPECT r42 = 0x0   shrp R = r2, r3, 25
// EXPECT r43 = 0x0   shl R = r2, 25
// EXPECT r44 = 0x0   shr.u R = r3, 25
// EXPECT r45 = 0x0   shr R = r3, 25
// EXPECT r46 = 0x0   extr.u R = r3, 26, 24
// EXPECT r47 = 0x0   extr R = r3, 26, 24
// EXPECT r48 = 0x0   dep.z R = r2, 26, 24
// EXPECT r49 = 0x3ffff90000000   dep.z R = -28, 26, 24
// EXPECT r50 = 0x2   dep R = r2, r3, 26, 13
// EXPECT r51 = 0x3fffffc000002   dep R = -1, r3, 26, 24
// EXPECT r52 = 0x0   shrp R = r2, r3, 2
// EXPECT r53 = 0x0   shl R = r2, 2
// EXPECT r54 = 0x0   shr.u R = r3, 2
// EXPECT r55 = 0x0   shr R = r3, 2
// EXPECT r56 = 0x0   extr.u R = r3, 46, 54
// EXPECT r57 = 0x0   extr R = r3, 46, 54
// EXPECT r58 = 0x0   dep.z R = r2, 46, 54
// EXPECT r59 = 0xfff5000000000000   dep.z R = -44, 46, 54
// EXPECT r60 = 0x2   dep R = r2, r3, 46, 5
// EXPECT r61 = 0xffffc00000000002   dep R = -1, r3, 46, 54
// EXPECT r62 = 0x0   shrp R = r2, r3, 8
// EXPECT r63 = 0x0   shl R = r2, 8
// EXPECT r64 = 0x0   shr.u R = r3, 8
// EXPECT r65 = 0x0   shr R = r3, 8
// EXPECT r66 = 0x0   extr.u R = r3, 42, 39
// EXPECT r67 = 0x0   extr R = r3, 42, 39
// EXPECT r68 = 0x0   dep.z R = r2, 42, 39
// EXPECT r69 = 0xfffe040000000000   dep.z R = -127, 42, 39
// EXPECT r70 = 0x2   dep R = r2, r3, 42, 11
// EXPECT r71 = 0x2   dep R = 0, r3, 42, 39
// EXPECT r72 = 0x0   shrp R = r2, r3, 39
// EXPECT r73 = 0x0   shl R = r2, 39
// EXPECT r74 = 0x0   shr.u R = r3, 39
// EXPECT r75 = 0x0   shr R = r3, 39
// EXPECT r76 = 0x0   extr.u R = r3, 45, 40
// EXPECT r77 = 0x0   extr R = r3, 45, 40
// EXPECT r78 = 0x0   dep.z R = r2, 45, 40
// EXPECT r79 = 0xec00000000000   dep.z R = 118, 45, 40
// EXPECT r80 = 0x2   dep R = r2, r3, 45, 11
// EXPECT r81 = 0x2   dep R = 0, r3, 45, 40
// EXPECT r82 = 0x0   shrp R = r2, r3, 61
// EXPECT r83 = 0x0   shl R = r2, 61
// EXPECT r84 = 0x0   shr.u R = r3, 61
// EXPECT r85 = 0x0   shr R = r3, 61
// EXPECT r86 = 0x0   extr.u R = r3, 60, 23
// EXPECT r87 = 0x0   extr R = r3, 60, 23
// EXPECT r88 = 0x0   dep.z R = r2, 60, 23
// EXPECT r89 = 0xd000000000000000   dep.z R = -99, 60, 23
// EXPECT r90 = 0x2   dep R = r2, r3, 60, 9
// EXPECT r91 = 0x2   dep R = 0, r3, 60, 23
// EXPECT r92 = 0x0   shrp R = r2, r3, 45
// EXPECT r93 = 0x0   shl R = r2, 45
// EXPECT r94 = 0x0   shr.u R = r3, 45
// EXPECT r95 = 0x0   shr R = r3, 45
// EXPECT r96 = 0x2   sxt1 R = r3
// EXPECT r97 = 0x2   sxt2 R = r3
// EXPECT r98 = 0x2   sxt4 R = r3
// EXPECT r99 = 0x2   zxt1 R = r3
// EXPECT r100 = 0x2   zxt2 R = r3
// EXPECT r101 = 0x2   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp4.eq P1, P2 = -68, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 1   cmp4.geu P1, P2 = 23, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp4.ne P1, P2 = -3, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.le P1, P2 = 64, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   tbit.z P1, P2 = r3, 26
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp.ge P1, P2 = -36, r3
// EXPECT p19 = 1
// EXPECT p20 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.lt P1, P2 = 86, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.geu P1, P2 = -28, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 0   cmp.leu P1, P2 = -124, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   tbit.z P1, P2 = r3, 11
// EXPECT p33 = 0
// EXPECT p34 = 1   tbit.z P1, P2 = r3, 55
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   tbit.nz P1, P2 = r3, 4
// EXPECT p41 = 1
// EXPECT p42 = 0   tbit.nz P1, P2 = r3, 11
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.eq P1, P2 = 78, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   tbit.nz P1, P2 = r3, 26
// EXPECT p49 = 1
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 4
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp.ltu P1, P2 = 59, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.gtu P1, P2 = -73, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x8000000000000000
  movl r3 = 0x2
  movl r4 = 0x1f
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -128, r3
  and r13 = -128, r3
  or r14 = -128, r3
  xor r15 = -128, r3
  andcm r16 = -128, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -8018, r3
  addl r22 = -340105, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -8018, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 27, 22
  extr r37 = r3, 27, 22
  dep.z r38 = r2, 27, 22
  dep.z r39 = -43, 27, 22
  dep r40 = r2, r3, 27, 10
  dep r41 = -1, r3, 27, 22
  shrp r42 = r2, r3, 25
  shl r43 = r2, 25
  shr.u r44 = r3, 25
  shr r45 = r3, 25
  extr.u r46 = r3, 26, 24
  extr r47 = r3, 26, 24
  dep.z r48 = r2, 26, 24
  dep.z r49 = -28, 26, 24
  dep r50 = r2, r3, 26, 13
  dep r51 = -1, r3, 26, 24
  shrp r52 = r2, r3, 2
  shl r53 = r2, 2
  shr.u r54 = r3, 2
  shr r55 = r3, 2
  extr.u r56 = r3, 46, 54
  extr r57 = r3, 46, 54
  dep.z r58 = r2, 46, 54
  dep.z r59 = -44, 46, 54
  dep r60 = r2, r3, 46, 5
  dep r61 = -1, r3, 46, 54
  shrp r62 = r2, r3, 8
  shl r63 = r2, 8
  shr.u r64 = r3, 8
  shr r65 = r3, 8
  extr.u r66 = r3, 42, 39
  extr r67 = r3, 42, 39
  dep.z r68 = r2, 42, 39
  dep.z r69 = -127, 42, 39
  dep r70 = r2, r3, 42, 11
  dep r71 = 0, r3, 42, 39
  shrp r72 = r2, r3, 39
  shl r73 = r2, 39
  shr.u r74 = r3, 39
  shr r75 = r3, 39
  extr.u r76 = r3, 45, 40
  extr r77 = r3, 45, 40
  dep.z r78 = r2, 45, 40
  dep.z r79 = 118, 45, 40
  dep r80 = r2, r3, 45, 11
  dep r81 = 0, r3, 45, 40
  shrp r82 = r2, r3, 61
  shl r83 = r2, 61
  shr.u r84 = r3, 61
  shr r85 = r3, 61
  extr.u r86 = r3, 60, 23
  extr r87 = r3, 60, 23
  dep.z r88 = r2, 60, 23
  dep.z r89 = -99, 60, 23
  dep r90 = r2, r3, 60, 9
  dep r91 = 0, r3, 60, 23
  shrp r92 = r2, r3, 45
  shl r93 = r2, 45
  shr.u r94 = r3, 45
  shr r95 = r3, 45
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.eq p6, p7 = -68, r3
  cmp4.gt p8, p9 = r2, r3
  cmp4.geu p10, p11 = 23, r3
  cmp4.ne p12, p13 = -3, r3
  cmp.le p14, p15 = 64, r3
  tbit.z p16, p17 = r3, 26
  cmp.ge p18, p19 = -36, r3
  cmp4.gtu p20, p21 = r2, r3
  cmp.lt p22, p23 = 86, r3
  cmp.geu p24, p25 = -28, r3
  cmp4.eq p26, p27 = r2, r3
  cmp4.geu p28, p29 = r2, r3
  cmp.leu p30, p31 = -124, r3
  tbit.z p32, p33 = r3, 11
  tbit.z p34, p35 = r3, 55
  cmp4.ge p36, p37 = r2, r3
  cmp4.leu p38, p39 = r2, r3
  tbit.nz p40, p41 = r3, 4
  tbit.nz p42, p43 = r3, 11
  cmp.eq p44, p45 = 78, r3
  cmp.ne p46, p47 = r2, r3
  tbit.nz p48, p49 = r3, 26
  tbit.z p50, p51 = r3, 4
  cmp.gt p52, p53 = r2, r3
  cmp4.le p54, p55 = r2, r3
  cmp.ltu p56, p57 = 59, r3
  cmp4.ltu p58, p59 = r2, r3
  cmp4.gtu p60, p61 = -73, r3
  cmp.le p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
