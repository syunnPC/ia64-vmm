/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x100000000   add R = r2, r3
// EXPECT r9 = 0x100000001   add R = r2, r3, 1
// EXPECT r10 = 0xfffffffe   sub R = r2, r3
// EXPECT r11 = 0xfffffffd   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffaf   sub R = -80, r3
// EXPECT r13 = 0x0   and R = -80, r3
// EXPECT r14 = 0xffffffffffffffb1   or R = -80, r3
// EXPECT r15 = 0xffffffffffffffb1   xor R = -80, r3
// EXPECT r16 = 0xffffffffffffffb0   andcm R = -80, r3
// EXPECT r17 = 0x1   and R = r2, r3
// EXPECT r18 = 0xffffffff   or R = r2, r3
// EXPECT r19 = 0xfffffffe   xor R = r2, r3
// EXPECT r20 = 0xfffffffe   andcm R = r2, r3
// EXPECT r21 = 0xffffffffffffeab9   adds R = -5448, r3
// EXPECT r22 = 0xfffffffffff85cfb   addl R = -500485, r1
// EXPECT r23 = 0x1ffffffff   shladd R = r2, 1, r3
// EXPECT r24 = 0xffffffff   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x3fffffffd   shladd R = r2, 2, r3
// EXPECT r26 = 0xfffffffd   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x7fffffff9   shladd R = r2, 3, r3
// EXPECT r28 = 0xfffffff9   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xffffffff1   shladd R = r2, 4, r3
// EXPECT r30 = 0xfffffff1   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x0   addp4 R = r2, r3
// EXPECT r32 = 0xffffeab9   addp4 R = -5448, r3
// EXPECT r33 = 0xffffffff00000000   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 32, 37
// EXPECT r37 = 0x0   extr R = r3, 32, 37
// EXPECT r38 = 0xffffffff00000000   dep.z R = r2, 32, 37
// EXPECT r39 = 0x1c00000000   dep.z R = 28, 32, 37
// EXPECT r40 = 0x1ff00000001   dep R = r2, r3, 32, 9
// EXPECT r41 = 0x1   dep R = 0, r3, 32, 37
// EXPECT r42 = 0xfffe000000000000   shrp R = r2, r3, 15
// EXPECT r43 = 0x7fffffff8000   shl R = r2, 15
// EXPECT r44 = 0x0   shr.u R = r3, 15
// EXPECT r45 = 0x0   shr R = r3, 15
// EXPECT r46 = 0x0   extr.u R = r3, 2, 35
// EXPECT r47 = 0x0   extr R = r3, 2, 35
// EXPECT r48 = 0x3fffffffc   dep.z R = r2, 2, 35
// EXPECT r49 = 0x1fffffffdc   dep.z R = -9, 2, 35
// EXPECT r50 = 0x3fd   dep R = r2, r3, 2, 8
// EXPECT r51 = 0x1   dep R = 0, r3, 2, 35
// EXPECT r52 = 0xfe00000000000000   shrp R = r2, r3, 7
// EXPECT r53 = 0x7fffffff80   shl R = r2, 7
// EXPECT r54 = 0x0   shr.u R = r3, 7
// EXPECT r55 = 0x0   shr R = r3, 7
// EXPECT r56 = 0x0   extr.u R = r3, 1, 39
// EXPECT r57 = 0x0   extr R = r3, 1, 39
// EXPECT r58 = 0x1fffffffe   dep.z R = r2, 1, 39
// EXPECT r59 = 0x1e   dep.z R = 15, 1, 39
// EXPECT r60 = 0x3f   dep R = r2, r3, 1, 5
// EXPECT r61 = 0xffffffffff   dep R = -1, r3, 1, 39
// EXPECT r62 = 0x7fffffff80   shrp R = r2, r3, 57
// EXPECT r63 = 0xfe00000000000000   shl R = r2, 57
// EXPECT r64 = 0x0   shr.u R = r3, 57
// EXPECT r65 = 0x0   shr R = r3, 57
// EXPECT r66 = 0x0   extr.u R = r3, 19, 36
// EXPECT r67 = 0x0   extr R = r3, 19, 36
// EXPECT r68 = 0x7fffffff80000   dep.z R = r2, 19, 36
// EXPECT r69 = 0x2e80000   dep.z R = 93, 19, 36
// EXPECT r70 = 0x1f80001   dep R = r2, r3, 19, 6
// EXPECT r71 = 0x1   dep R = 0, r3, 19, 36
// EXPECT r72 = 0x1fffffffe000   shrp R = r2, r3, 51
// EXPECT r73 = 0xfff8000000000000   shl R = r2, 51
// EXPECT r74 = 0x0   shr.u R = r3, 51
// EXPECT r75 = 0x0   shr R = r3, 51
// EXPECT r76 = 0x0   extr.u R = r3, 15, 17
// EXPECT r77 = 0x0   extr R = r3, 15, 17
// EXPECT r78 = 0xffff8000   dep.z R = r2, 15, 17
// EXPECT r79 = 0x2d8000   dep.z R = 91, 15, 17
// EXPECT r80 = 0x7ff8001   dep R = r2, r3, 15, 12
// EXPECT r81 = 0x1   dep R = 0, r3, 15, 17
// EXPECT r82 = 0xfffc000000000000   shrp R = r2, r3, 14
// EXPECT r83 = 0x3fffffffc000   shl R = r2, 14
// EXPECT r84 = 0x0   shr.u R = r3, 14
// EXPECT r85 = 0x0   shr R = r3, 14
// EXPECT r86 = 0x0   extr.u R = r3, 6, 25
// EXPECT r87 = 0x0   extr R = r3, 6, 25
// EXPECT r88 = 0x7fffffc0   dep.z R = r2, 6, 25
// EXPECT r89 = 0x17c0   dep.z R = 95, 6, 25
// EXPECT r90 = 0xfc1   dep R = r2, r3, 6, 6
// EXPECT r91 = 0x1   dep R = 0, r3, 6, 25
// EXPECT r92 = 0xffc0000000000000   shrp R = r2, r3, 10
// EXPECT r93 = 0x3fffffffc00   shl R = r2, 10
// EXPECT r94 = 0x0   shr.u R = r3, 10
// EXPECT r95 = 0x0   shr R = r3, 10
// EXPECT r96 = 0x1   sxt1 R = r3
// EXPECT r97 = 0x1   sxt2 R = r3
// EXPECT r98 = 0x1   sxt4 R = r3
// EXPECT r99 = 0x1   zxt1 R = r3
// EXPECT r100 = 0x1   zxt2 R = r3
// EXPECT r101 = 0x1   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp.eq P1, P2 = -70, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   tbit.nz P1, P2 = r3, 24
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp4.gt P1, P2 = 3, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp.geu P1, P2 = 105, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   tbit.z P1, P2 = r3, 24
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   cmp.ge P1, P2 = -68, r3
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp.gt P1, P2 = -101, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.gtu P1, P2 = -44, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.le P1, P2 = 77, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.geu P1, P2 = -106, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   tbit.nz P1, P2 = r3, 28
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.le P1, P2 = -81, r3
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   cmp.lt P1, P2 = -127, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp4.leu P1, P2 = 42, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0xffffffff
  movl r3 = 0x1
  movl r4 = 0x20
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -80, r3
  and r13 = -80, r3
  or r14 = -80, r3
  xor r15 = -80, r3
  andcm r16 = -80, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -5448, r3
  addl r22 = -500485, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -5448, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 32, 37
  extr r37 = r3, 32, 37
  dep.z r38 = r2, 32, 37
  dep.z r39 = 28, 32, 37
  dep r40 = r2, r3, 32, 9
  dep r41 = 0, r3, 32, 37
  shrp r42 = r2, r3, 15
  shl r43 = r2, 15
  shr.u r44 = r3, 15
  shr r45 = r3, 15
  extr.u r46 = r3, 2, 35
  extr r47 = r3, 2, 35
  dep.z r48 = r2, 2, 35
  dep.z r49 = -9, 2, 35
  dep r50 = r2, r3, 2, 8
  dep r51 = 0, r3, 2, 35
  shrp r52 = r2, r3, 7
  shl r53 = r2, 7
  shr.u r54 = r3, 7
  shr r55 = r3, 7
  extr.u r56 = r3, 1, 39
  extr r57 = r3, 1, 39
  dep.z r58 = r2, 1, 39
  dep.z r59 = 15, 1, 39
  dep r60 = r2, r3, 1, 5
  dep r61 = -1, r3, 1, 39
  shrp r62 = r2, r3, 57
  shl r63 = r2, 57
  shr.u r64 = r3, 57
  shr r65 = r3, 57
  extr.u r66 = r3, 19, 36
  extr r67 = r3, 19, 36
  dep.z r68 = r2, 19, 36
  dep.z r69 = 93, 19, 36
  dep r70 = r2, r3, 19, 6
  dep r71 = 0, r3, 19, 36
  shrp r72 = r2, r3, 51
  shl r73 = r2, 51
  shr.u r74 = r3, 51
  shr r75 = r3, 51
  extr.u r76 = r3, 15, 17
  extr r77 = r3, 15, 17
  dep.z r78 = r2, 15, 17
  dep.z r79 = 91, 15, 17
  dep r80 = r2, r3, 15, 12
  dep r81 = 0, r3, 15, 17
  shrp r82 = r2, r3, 14
  shl r83 = r2, 14
  shr.u r84 = r3, 14
  shr r85 = r3, 14
  extr.u r86 = r3, 6, 25
  extr r87 = r3, 6, 25
  dep.z r88 = r2, 6, 25
  dep.z r89 = 95, 6, 25
  dep r90 = r2, r3, 6, 6
  dep r91 = 0, r3, 6, 25
  shrp r92 = r2, r3, 10
  shl r93 = r2, 10
  shr.u r94 = r3, 10
  shr r95 = r3, 10
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.leu p6, p7 = r2, r3
  cmp.eq p8, p9 = -70, r3
  tbit.nz p10, p11 = r3, 24
  cmp4.gt p12, p13 = 3, r3
  cmp.le p14, p15 = r2, r3
  cmp.geu p16, p17 = 105, r3
  tbit.z p18, p19 = r3, 24
  cmp4.ltu p20, p21 = r2, r3
  cmp.ge p22, p23 = -68, r3
  cmp.ltu p24, p25 = r2, r3
  cmp4.lt p26, p27 = r2, r3
  cmp4.geu p28, p29 = r2, r3
  cmp.gt p30, p31 = -101, r3
  cmp.gtu p32, p33 = -44, r3
  cmp.gt p34, p35 = r2, r3
  cmp.gtu p36, p37 = r2, r3
  cmp4.ge p38, p39 = r2, r3
  cmp.le p40, p41 = 77, r3
  cmp.lt p42, p43 = r2, r3
  cmp4.geu p44, p45 = -106, r3
  tbit.nz p46, p47 = r3, 28
  cmp4.le p48, p49 = -81, r3
  cmp4.eq p50, p51 = r2, r3
  cmp.ne p52, p53 = r2, r3
  cmp4.gt p54, p55 = r2, r3
  cmp4.gtu p56, p57 = r2, r3
  cmp.eq p58, p59 = r2, r3
  cmp.lt p60, p61 = -127, r3
  cmp4.leu p62, p63 = 42, r3
  ;;
  break.i 0x1ffffe
