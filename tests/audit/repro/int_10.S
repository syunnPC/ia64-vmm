/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1137a9777   add R = r2, r3
// EXPECT r9 = 0x1137a9778   add R = r2, r3, 1
// EXPECT r10 = 0xec856889   sub R = r2, r3
// EXPECT r11 = 0xec856888   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffec85689a   sub R = 17, r3
// EXPECT r13 = 0x11   and R = 17, r3
// EXPECT r14 = 0x137a9777   or R = 17, r3
// EXPECT r15 = 0x137a9766   xor R = 17, r3
// EXPECT r16 = 0x0   andcm R = 17, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x1137a9777   or R = r2, r3
// EXPECT r19 = 0x1137a9777   xor R = r2, r3
// EXPECT r20 = 0x100000000   andcm R = r2, r3
// EXPECT r21 = 0x137a990a   adds R = 403, r3
// EXPECT r22 = 0x1a7971   addl R = 1735025, r1
// EXPECT r23 = 0x2137a9777   shladd R = r2, 1, r3
// EXPECT r24 = 0x137a9777   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x4137a9777   shladd R = r2, 2, r3
// EXPECT r26 = 0x137a9777   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x8137a9777   shladd R = r2, 3, r3
// EXPECT r28 = 0x137a9777   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x10137a9777   shladd R = r2, 4, r3
// EXPECT r30 = 0x137a9777   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x137a9777   addp4 R = r2, r3
// EXPECT r32 = 0x137a990a   addp4 R = 403, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x4d   extr.u R = r3, 22, 39
// EXPECT r37 = 0x4d   extr R = r3, 22, 39
// EXPECT r38 = 0x40000000000000   dep.z R = r2, 22, 39
// EXPECT r39 = 0xe400000   dep.z R = 57, 22, 39
// EXPECT r40 = 0x103a9777   dep R = r2, r3, 22, 5
// EXPECT r41 = 0x1ffffffffffa9777   dep R = -1, r3, 22, 39
// EXPECT r42 = 0x0   shrp R = r2, r3, 30
// EXPECT r43 = 0x4000000000000000   shl R = r2, 30
// EXPECT r44 = 0x0   shr.u R = r3, 30
// EXPECT r45 = 0x0   shr R = r3, 30
// EXPECT r46 = 0x0   extr.u R = r3, 56, 49
// EXPECT r47 = 0x0   extr R = r3, 56, 49
// EXPECT r48 = 0x0   dep.z R = r2, 56, 49
// EXPECT r49 = 0x9600000000000000   dep.z R = -106, 56, 49
// EXPECT r50 = 0x137a9777   dep R = r2, r3, 56, 1
// EXPECT r51 = 0x137a9777   dep R = 0, r3, 56, 49
// EXPECT r52 = 0x9bd   shrp R = r2, r3, 17
// EXPECT r53 = 0x2000000000000   shl R = r2, 17
// EXPECT r54 = 0x9bd   shr.u R = r3, 17
// EXPECT r55 = 0x9bd   shr R = r3, 17
// EXPECT r56 = 0x13   extr.u R = r3, 24, 39
// EXPECT r57 = 0x13   extr R = r3, 24, 39
// EXPECT r58 = 0x100000000000000   dep.z R = r2, 24, 39
// EXPECT r59 = 0x3b000000   dep.z R = 59, 24, 39
// EXPECT r60 = 0x7a9777   dep R = r2, r3, 24, 8
// EXPECT r61 = 0x7fffffffff7a9777   dep R = -1, r3, 24, 39
// EXPECT r62 = 0x8000000000   shrp R = r2, r3, 57
// EXPECT r63 = 0x0   shl R = r2, 57
// EXPECT r64 = 0x0   shr.u R = r3, 57
// EXPECT r65 = 0x0   shr R = r3, 57
// EXPECT r66 = 0x0   extr.u R = r3, 55, 61
// EXPECT r67 = 0x0   extr R = r3, 55, 61
// EXPECT r68 = 0x0   dep.z R = r2, 55, 61
// EXPECT r69 = 0xd080000000000000   dep.z R = -95, 55, 61
// EXPECT r70 = 0x137a9777   dep R = r2, r3, 55, 11
// EXPECT r71 = 0x137a9777   dep R = 0, r3, 55, 61
// EXPECT r72 = 0x1   shrp R = r2, r3, 28
// EXPECT r73 = 0x1000000000000000   shl R = r2, 28
// EXPECT r74 = 0x1   shr.u R = r3, 28
// EXPECT r75 = 0x1   shr R = r3, 28
// EXPECT r76 = 0x0   extr.u R = r3, 52, 31
// EXPECT r77 = 0x0   extr R = r3, 52, 31
// EXPECT r78 = 0x0   dep.z R = r2, 52, 31
// EXPECT r79 = 0xf920000000000000   dep.z R = -110, 52, 31
// EXPECT r80 = 0x137a9777   dep R = r2, r3, 52, 2
// EXPECT r81 = 0xfff00000137a9777   dep R = -1, r3, 52, 31
// EXPECT r82 = 0x400000000000000   shrp R = r2, r3, 38
// EXPECT r83 = 0x0   shl R = r2, 38
// EXPECT r84 = 0x0   shr.u R = r3, 38
// EXPECT r85 = 0x0   shr R = r3, 38
// EXPECT r86 = 0x54b   extr.u R = r3, 9, 11
// EXPECT r87 = 0xfffffffffffffd4b   extr R = r3, 9, 11
// EXPECT r88 = 0x0   dep.z R = r2, 9, 11
// EXPECT r89 = 0xf9800   dep.z R = -52, 9, 11
// EXPECT r90 = 0x13400177   dep R = r2, r3, 9, 13
// EXPECT r91 = 0x137fff77   dep R = -1, r3, 9, 11
// EXPECT r92 = 0x26f   shrp R = r2, r3, 19
// EXPECT r93 = 0x8000000000000   shl R = r2, 19
// EXPECT r94 = 0x26f   shr.u R = r3, 19
// EXPECT r95 = 0x26f   shr R = r3, 19
// EXPECT r96 = 0x77   sxt1 R = r3
// EXPECT r97 = 0xffffffffffff9777   sxt2 R = r3
// EXPECT r98 = 0x137a9777   sxt4 R = r3
// EXPECT r99 = 0x77   zxt1 R = r3
// EXPECT r100 = 0x9777   zxt2 R = r3
// EXPECT r101 = 0x137a9777   zxt4 R = r3
// EXPECT r102 = 0x13   popcnt R = r3
// EXPECT p6 = 1   cmp.gtu P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.eq P1, P2 = 12, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp.gtu P1, P2 = 84, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.lt P1, P2 = 98, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp4.lt P1, P2 = -37, r3
// EXPECT p27 = 0
// EXPECT p28 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 1   cmp4.leu P1, P2 = 105, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   tbit.z P1, P2 = r3, 61
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp.ge P1, P2 = 100, r3
// EXPECT p37 = 1
// EXPECT p38 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 1   cmp.geu P1, P2 = -3, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.gt P1, P2 = 26, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   tbit.nz P1, P2 = r3, 41
// EXPECT p45 = 1
// EXPECT p46 = 0   tbit.nz P1, P2 = r3, 61
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.le P1, P2 = 8, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 44
// EXPECT p51 = 0
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 55
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 0   cmp.gt P1, P2 = 93, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x100000000
  movl r3 = 0x137a9777
  movl r4 = 0x41
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 17, r3
  and r13 = 17, r3
  or r14 = 17, r3
  xor r15 = 17, r3
  andcm r16 = 17, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 403, r3
  addl r22 = 1735025, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 403, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 22, 39
  extr r37 = r3, 22, 39
  dep.z r38 = r2, 22, 39
  dep.z r39 = 57, 22, 39
  dep r40 = r2, r3, 22, 5
  dep r41 = -1, r3, 22, 39
  shrp r42 = r2, r3, 30
  shl r43 = r2, 30
  shr.u r44 = r3, 30
  shr r45 = r3, 30
  extr.u r46 = r3, 56, 49
  extr r47 = r3, 56, 49
  dep.z r48 = r2, 56, 49
  dep.z r49 = -106, 56, 49
  dep r50 = r2, r3, 56, 1
  dep r51 = 0, r3, 56, 49
  shrp r52 = r2, r3, 17
  shl r53 = r2, 17
  shr.u r54 = r3, 17
  shr r55 = r3, 17
  extr.u r56 = r3, 24, 39
  extr r57 = r3, 24, 39
  dep.z r58 = r2, 24, 39
  dep.z r59 = 59, 24, 39
  dep r60 = r2, r3, 24, 8
  dep r61 = -1, r3, 24, 39
  shrp r62 = r2, r3, 57
  shl r63 = r2, 57
  shr.u r64 = r3, 57
  shr r65 = r3, 57
  extr.u r66 = r3, 55, 61
  extr r67 = r3, 55, 61
  dep.z r68 = r2, 55, 61
  dep.z r69 = -95, 55, 61
  dep r70 = r2, r3, 55, 11
  dep r71 = 0, r3, 55, 61
  shrp r72 = r2, r3, 28
  shl r73 = r2, 28
  shr.u r74 = r3, 28
  shr r75 = r3, 28
  extr.u r76 = r3, 52, 31
  extr r77 = r3, 52, 31
  dep.z r78 = r2, 52, 31
  dep.z r79 = -110, 52, 31
  dep r80 = r2, r3, 52, 2
  dep r81 = -1, r3, 52, 31
  shrp r82 = r2, r3, 38
  shl r83 = r2, 38
  shr.u r84 = r3, 38
  shr r85 = r3, 38
  extr.u r86 = r3, 9, 11
  extr r87 = r3, 9, 11
  dep.z r88 = r2, 9, 11
  dep.z r89 = -52, 9, 11
  dep r90 = r2, r3, 9, 13
  dep r91 = -1, r3, 9, 11
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
  cmp.gtu p6, p7 = r2, r3
  cmp.eq p8, p9 = r2, r3
  cmp4.gtu p10, p11 = r2, r3
  cmp.ne p12, p13 = r2, r3
  cmp4.eq p14, p15 = 12, r3
  cmp4.eq p16, p17 = r2, r3
  cmp.gtu p18, p19 = 84, r3
  cmp.lt p20, p21 = 98, r3
  cmp.ge p22, p23 = r2, r3
  cmp4.le p24, p25 = r2, r3
  cmp4.lt p26, p27 = -37, r3
  cmp.leu p28, p29 = r2, r3
  cmp4.lt p30, p31 = r2, r3
  cmp4.leu p32, p33 = 105, r3
  tbit.z p34, p35 = r3, 61
  cmp.ge p36, p37 = 100, r3
  cmp4.leu p38, p39 = r2, r3
  cmp.geu p40, p41 = -3, r3
  cmp4.gt p42, p43 = 26, r3
  tbit.nz p44, p45 = r3, 41
  tbit.nz p46, p47 = r3, 61
  cmp4.le p48, p49 = 8, r3
  tbit.z p50, p51 = r3, 44
  tbit.z p52, p53 = r3, 55
  cmp.gt p54, p55 = r2, r3
  cmp.lt p56, p57 = r2, r3
  cmp.gt p58, p59 = 93, r3
  cmp4.gt p60, p61 = r2, r3
  cmp.geu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
