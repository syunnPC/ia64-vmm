/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x2   add R = r2, r3
// EXPECT r9 = 0x3   add R = r2, r3, 1
// EXPECT r10 = 0xfffffffffffffffe   sub R = r2, r3
// EXPECT r11 = 0xfffffffffffffffd   sub R = r2, r3, 1
// EXPECT r12 = 0x32   sub R = 52, r3
// EXPECT r13 = 0x0   and R = 52, r3
// EXPECT r14 = 0x36   or R = 52, r3
// EXPECT r15 = 0x36   xor R = 52, r3
// EXPECT r16 = 0x34   andcm R = 52, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x2   or R = r2, r3
// EXPECT r19 = 0x2   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0xfffffffffffffd09   adds R = -761, r3
// EXPECT r22 = 0xfffffffffff4e5f9   addl R = -727559, r1
// EXPECT r23 = 0x2   shladd R = r2, 1, r3
// EXPECT r24 = 0x2   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x2   shladd R = r2, 2, r3
// EXPECT r26 = 0x2   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x2   shladd R = r2, 3, r3
// EXPECT r28 = 0x2   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x2   shladd R = r2, 4, r3
// EXPECT r30 = 0x2   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x2   addp4 R = r2, r3
// EXPECT r32 = 0xfffffd09   addp4 R = -761, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x2   shr.u R = r3, r4
// EXPECT r35 = 0x2   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 59, 40
// EXPECT r37 = 0x0   extr R = r3, 59, 40
// EXPECT r38 = 0x0   dep.z R = r2, 59, 40
// EXPECT r39 = 0xe000000000000000   dep.z R = 124, 59, 40
// EXPECT r40 = 0x2   dep R = r2, r3, 59, 6
// EXPECT r41 = 0xf800000000000002   dep R = -1, r3, 59, 40
// EXPECT r42 = 0x0   shrp R = r2, r3, 10
// EXPECT r43 = 0x0   shl R = r2, 10
// EXPECT r44 = 0x0   shr.u R = r3, 10
// EXPECT r45 = 0x0   shr R = r3, 10
// EXPECT r46 = 0x0   extr.u R = r3, 4, 38
// EXPECT r47 = 0x0   extr R = r3, 4, 38
// EXPECT r48 = 0x0   dep.z R = r2, 4, 38
// EXPECT r49 = 0x3ffffffff50   dep.z R = -11, 4, 38
// EXPECT r50 = 0x2   dep R = r2, r3, 4, 10
// EXPECT r51 = 0x3fffffffff2   dep R = -1, r3, 4, 38
// EXPECT r52 = 0x0   shrp R = r2, r3, 10
// EXPECT r53 = 0x0   shl R = r2, 10
// EXPECT r54 = 0x0   shr.u R = r3, 10
// EXPECT r55 = 0x0   shr R = r3, 10
// EXPECT r56 = 0x0   extr.u R = r3, 31, 52
// EXPECT r57 = 0x0   extr R = r3, 31, 52
// EXPECT r58 = 0x0   dep.z R = r2, 31, 52
// EXPECT r59 = 0xa00000000   dep.z R = 20, 31, 52
// EXPECT r60 = 0x2   dep R = r2, r3, 31, 14
// EXPECT r61 = 0x2   dep R = 0, r3, 31, 52
// EXPECT r62 = 0x0   shrp R = r2, r3, 61
// EXPECT r63 = 0x0   shl R = r2, 61
// EXPECT r64 = 0x0   shr.u R = r3, 61
// EXPECT r65 = 0x0   shr R = r3, 61
// EXPECT r66 = 0x1   extr.u R = r3, 1, 24
// EXPECT r67 = 0x1   extr R = r3, 1, 24
// EXPECT r68 = 0x0   dep.z R = r2, 1, 24
// EXPECT r69 = 0xc8   dep.z R = 100, 1, 24
// EXPECT r70 = 0x0   dep R = r2, r3, 1, 6
// EXPECT r71 = 0x0   dep R = 0, r3, 1, 24
// EXPECT r72 = 0x0   shrp R = r2, r3, 7
// EXPECT r73 = 0x0   shl R = r2, 7
// EXPECT r74 = 0x0   shr.u R = r3, 7
// EXPECT r75 = 0x0   shr R = r3, 7
// EXPECT r76 = 0x0   extr.u R = r3, 16, 34
// EXPECT r77 = 0x0   extr R = r3, 16, 34
// EXPECT r78 = 0x0   dep.z R = r2, 16, 34
// EXPECT r79 = 0x3ffffff980000   dep.z R = -104, 16, 34
// EXPECT r80 = 0x2   dep R = r2, r3, 16, 10
// EXPECT r81 = 0x2   dep R = 0, r3, 16, 34
// EXPECT r82 = 0x0   shrp R = r2, r3, 49
// EXPECT r83 = 0x0   shl R = r2, 49
// EXPECT r84 = 0x0   shr.u R = r3, 49
// EXPECT r85 = 0x0   shr R = r3, 49
// EXPECT r86 = 0x0   extr.u R = r3, 7, 25
// EXPECT r87 = 0x0   extr R = r3, 7, 25
// EXPECT r88 = 0x0   dep.z R = r2, 7, 25
// EXPECT r89 = 0xffffc900   dep.z R = -110, 7, 25
// EXPECT r90 = 0x2   dep R = r2, r3, 7, 16
// EXPECT r91 = 0x2   dep R = 0, r3, 7, 25
// EXPECT r92 = 0x0   shrp R = r2, r3, 41
// EXPECT r93 = 0x0   shl R = r2, 41
// EXPECT r94 = 0x0   shr.u R = r3, 41
// EXPECT r95 = 0x0   shr R = r3, 41
// EXPECT r96 = 0x2   sxt1 R = r3
// EXPECT r97 = 0x2   sxt2 R = r3
// EXPECT r98 = 0x2   sxt4 R = r3
// EXPECT r99 = 0x2   zxt1 R = r3
// EXPECT r100 = 0x2   zxt2 R = r3
// EXPECT r101 = 0x2   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp4.le P1, P2 = -58, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.lt P1, P2 = 51, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp.lt P1, P2 = 117, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.ltu P1, P2 = 61, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp4.ne P1, P2 = -63, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.geu P1, P2 = 61, r3
// EXPECT p23 = 0
// EXPECT p24 = 0   tbit.nz P1, P2 = r3, 40
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp.gt P1, P2 = -13, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   tbit.nz P1, P2 = r3, 0
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p43 = 0
// EXPECT p44 = 0   cmp4.leu P1, P2 = -2, r3
// EXPECT p45 = 1
// EXPECT p46 = 0   cmp.eq P1, P2 = 56, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.geu P1, P2 = -73, r3
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.nz P1, P2 = r3, 21
// EXPECT p55 = 1
// EXPECT p56 = 0   tbit.nz P1, P2 = r3, 53
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   tbit.z P1, P2 = r3, 0
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x0
  movl r3 = 0x2
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 52, r3
  and r13 = 52, r3
  or r14 = 52, r3
  xor r15 = 52, r3
  andcm r16 = 52, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -761, r3
  addl r22 = -727559, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -761, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 59, 40
  extr r37 = r3, 59, 40
  dep.z r38 = r2, 59, 40
  dep.z r39 = 124, 59, 40
  dep r40 = r2, r3, 59, 6
  dep r41 = -1, r3, 59, 40
  shrp r42 = r2, r3, 10
  shl r43 = r2, 10
  shr.u r44 = r3, 10
  shr r45 = r3, 10
  extr.u r46 = r3, 4, 38
  extr r47 = r3, 4, 38
  dep.z r48 = r2, 4, 38
  dep.z r49 = -11, 4, 38
  dep r50 = r2, r3, 4, 10
  dep r51 = -1, r3, 4, 38
  shrp r52 = r2, r3, 10
  shl r53 = r2, 10
  shr.u r54 = r3, 10
  shr r55 = r3, 10
  extr.u r56 = r3, 31, 52
  extr r57 = r3, 31, 52
  dep.z r58 = r2, 31, 52
  dep.z r59 = 20, 31, 52
  dep r60 = r2, r3, 31, 14
  dep r61 = 0, r3, 31, 52
  shrp r62 = r2, r3, 61
  shl r63 = r2, 61
  shr.u r64 = r3, 61
  shr r65 = r3, 61
  extr.u r66 = r3, 1, 24
  extr r67 = r3, 1, 24
  dep.z r68 = r2, 1, 24
  dep.z r69 = 100, 1, 24
  dep r70 = r2, r3, 1, 6
  dep r71 = 0, r3, 1, 24
  shrp r72 = r2, r3, 7
  shl r73 = r2, 7
  shr.u r74 = r3, 7
  shr r75 = r3, 7
  extr.u r76 = r3, 16, 34
  extr r77 = r3, 16, 34
  dep.z r78 = r2, 16, 34
  dep.z r79 = -104, 16, 34
  dep r80 = r2, r3, 16, 10
  dep r81 = 0, r3, 16, 34
  shrp r82 = r2, r3, 49
  shl r83 = r2, 49
  shr.u r84 = r3, 49
  shr r85 = r3, 49
  extr.u r86 = r3, 7, 25
  extr r87 = r3, 7, 25
  dep.z r88 = r2, 7, 25
  dep.z r89 = -110, 7, 25
  dep r90 = r2, r3, 7, 16
  dep r91 = 0, r3, 7, 25
  shrp r92 = r2, r3, 41
  shl r93 = r2, 41
  shr.u r94 = r3, 41
  shr r95 = r3, 41
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.gtu p6, p7 = r2, r3
  cmp4.le p8, p9 = -58, r3
  cmp4.lt p10, p11 = 51, r3
  cmp.lt p12, p13 = 117, r3
  cmp4.eq p14, p15 = r2, r3
  cmp4.ltu p16, p17 = 61, r3
  cmp.leu p18, p19 = r2, r3
  cmp4.ne p20, p21 = -63, r3
  cmp.geu p22, p23 = 61, r3
  tbit.nz p24, p25 = r3, 40
  cmp4.gt p26, p27 = r2, r3
  cmp.gt p28, p29 = -13, r3
  cmp.le p30, p31 = r2, r3
  cmp.ge p32, p33 = r2, r3
  cmp4.leu p34, p35 = r2, r3
  tbit.nz p36, p37 = r3, 0
  cmp4.lt p38, p39 = r2, r3
  cmp.ne p40, p41 = r2, r3
  cmp4.ltu p42, p43 = r2, r3
  cmp4.leu p44, p45 = -2, r3
  cmp.eq p46, p47 = 56, r3
  cmp4.geu p48, p49 = -73, r3
  cmp4.geu p50, p51 = r2, r3
  cmp.eq p52, p53 = r2, r3
  tbit.nz p54, p55 = r3, 21
  tbit.nz p56, p57 = r3, 53
  cmp.ltu p58, p59 = r2, r3
  cmp4.gtu p60, p61 = r2, r3
  tbit.z p62, p63 = r3, 0
  ;;
  break.i 0x1ffffe
