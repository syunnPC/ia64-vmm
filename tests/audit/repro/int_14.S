/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x4375d0349e4f6f2a   add R = r2, r3
// EXPECT r9 = 0x4375d0349e4f6f2b   add R = r2, r3, 1
// EXPECT r10 = 0xbc8a2fcc61b090d6   sub R = r2, r3
// EXPECT r11 = 0xbc8a2fcc61b090d5   sub R = r2, r3, 1
// EXPECT r12 = 0xbc8a2fcbe1b09079   sub R = -93, r3
// EXPECT r13 = 0x4375d0341e4f6f22   and R = -93, r3
// EXPECT r14 = 0xffffffffffffffab   or R = -93, r3
// EXPECT r15 = 0xbc8a2fcbe1b09089   xor R = -93, r3
// EXPECT r16 = 0xbc8a2fcbe1b09081   andcm R = -93, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x4375d0349e4f6f2a   or R = r2, r3
// EXPECT r19 = 0x4375d0349e4f6f2a   xor R = r2, r3
// EXPECT r20 = 0x80000000   andcm R = r2, r3
// EXPECT r21 = 0x4375d0341e4f5ee9   adds R = -4161, r3
// EXPECT r22 = 0xb24fc   addl R = 730364, r1
// EXPECT r23 = 0x4375d0351e4f6f2a   shladd R = r2, 1, r3
// EXPECT r24 = 0x1e4f6f2a   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x4375d0361e4f6f2a   shladd R = r2, 2, r3
// EXPECT r26 = 0x1e4f6f2a   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x4375d0381e4f6f2a   shladd R = r2, 3, r3
// EXPECT r28 = 0x1e4f6f2a   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x4375d03c1e4f6f2a   shladd R = r2, 4, r3
// EXPECT r30 = 0x1e4f6f2a   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x9e4f6f2a   addp4 R = r2, r3
// EXPECT r32 = 0x1e4f5ee9   addp4 R = -4161, r3
// EXPECT r33 = 0x80000000   shl R = r2, r4
// EXPECT r34 = 0x4375d0341e4f6f2a   shr.u R = r3, r4
// EXPECT r35 = 0x4375d0341e4f6f2a   shr R = r3, r4
// EXPECT r36 = 0x5e5   extr.u R = r3, 3, 11
// EXPECT r37 = 0xfffffffffffffde5   extr R = r3, 3, 11
// EXPECT r38 = 0x0   dep.z R = r2, 3, 11
// EXPECT r39 = 0x60   dep.z R = 12, 3, 11
// EXPECT r40 = 0x4375d0341e4f6c02   dep R = r2, r3, 3, 7
// EXPECT r41 = 0x4375d0341e4f7ffa   dep R = -1, r3, 3, 11
// EXPECT r42 = 0x10000000086e   shrp R = r2, r3, 51
// EXPECT r43 = 0x0   shl R = r2, 51
// EXPECT r44 = 0x86e   shr.u R = r3, 51
// EXPECT r45 = 0x86e   shr R = r3, 51
// EXPECT r46 = 0x43   extr.u R = r3, 56, 13
// EXPECT r47 = 0x43   extr R = r3, 56, 13
// EXPECT r48 = 0x0   dep.z R = r2, 56, 13
// EXPECT r49 = 0xb900000000000000   dep.z R = -71, 56, 13
// EXPECT r50 = 0x75d0341e4f6f2a   dep R = r2, r3, 56, 12
// EXPECT r51 = 0x75d0341e4f6f2a   dep R = 0, r3, 56, 13
// EXPECT r52 = 0x4375d0341e4f6   shrp R = r2, r3, 12
// EXPECT r53 = 0x80000000000   shl R = r2, 12
// EXPECT r54 = 0x4375d0341e4f6   shr.u R = r3, 12
// EXPECT r55 = 0x4375d0341e4f6   shr R = r3, 12
// EXPECT r56 = 0x1   extr.u R = r3, 62, 25
// EXPECT r57 = 0x1   extr R = r3, 62, 25
// EXPECT r58 = 0x0   dep.z R = r2, 62, 25
// EXPECT r59 = 0x8000000000000000   dep.z R = 10, 62, 25
// EXPECT r60 = 0x375d0341e4f6f2a   dep R = r2, r3, 62, 15
// EXPECT r61 = 0x375d0341e4f6f2a   dep R = 0, r3, 62, 25
// EXPECT r62 = 0x200000001   shrp R = r2, r3, 62
// EXPECT r63 = 0x0   shl R = r2, 62
// EXPECT r64 = 0x1   shr.u R = r3, 62
// EXPECT r65 = 0x1   shr R = r3, 62
// EXPECT r66 = 0x4375d03   extr.u R = r3, 36, 33
// EXPECT r67 = 0x4375d03   extr R = r3, 36, 33
// EXPECT r68 = 0x0   dep.z R = r2, 36, 33
// EXPECT r69 = 0xfffffb4000000000   dep.z R = -76, 36, 33
// EXPECT r70 = 0x4375d0041e4f6f2a   dep R = r2, r3, 36, 4
// EXPECT r71 = 0x41e4f6f2a   dep R = 0, r3, 36, 33
// EXPECT r72 = 0x10000000086eba06   shrp R = r2, r3, 35
// EXPECT r73 = 0x0   shl R = r2, 35
// EXPECT r74 = 0x86eba06   shr.u R = r3, 35
// EXPECT r75 = 0x86eba06   shr R = r3, 35
// EXPECT r76 = 0xba06   extr.u R = r3, 35, 16
// EXPECT r77 = 0xffffffffffffba06   extr R = r3, 35, 16
// EXPECT r78 = 0x0   dep.z R = r2, 35, 16
// EXPECT r79 = 0x7fc7000000000   dep.z R = -114, 35, 16
// EXPECT r80 = 0x4375d0041e4f6f2a   dep R = r2, r3, 35, 6
// EXPECT r81 = 0x4377fffc1e4f6f2a   dep R = -1, r3, 35, 16
// EXPECT r82 = 0x10dd740d0793d   shrp R = r2, r3, 14
// EXPECT r83 = 0x200000000000   shl R = r2, 14
// EXPECT r84 = 0x10dd740d0793d   shr.u R = r3, 14
// EXPECT r85 = 0x10dd740d0793d   shr R = r3, 14
// EXPECT r86 = 0x86eba0683c9ed   extr.u R = r3, 11, 55
// EXPECT r87 = 0x86eba0683c9ed   extr R = r3, 11, 55
// EXPECT r88 = 0x40000000000   dep.z R = r2, 11, 55
// EXPECT r89 = 0x3a800   dep.z R = 117, 11, 55
// EXPECT r90 = 0x4375d0341e4e072a   dep R = r2, r3, 11, 6
// EXPECT r91 = 0xffffffffffffff2a   dep R = -1, r3, 11, 55
// EXPECT r92 = 0x2000000010   shrp R = r2, r3, 58
// EXPECT r93 = 0x0   shl R = r2, 58
// EXPECT r94 = 0x10   shr.u R = r3, 58
// EXPECT r95 = 0x10   shr R = r3, 58
// EXPECT r96 = 0x2a   sxt1 R = r3
// EXPECT r97 = 0x6f2a   sxt2 R = r3
// EXPECT r98 = 0x1e4f6f2a   sxt4 R = r3
// EXPECT r99 = 0x2a   zxt1 R = r3
// EXPECT r100 = 0x6f2a   zxt2 R = r3
// EXPECT r101 = 0x1e4f6f2a   zxt4 R = r3
// EXPECT r102 = 0x20   popcnt R = r3
// EXPECT p6 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   cmp.geu P1, P2 = -8, r3
// EXPECT p11 = 0
// EXPECT p12 = 0   cmp4.gt P1, P2 = 97, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   tbit.nz P1, P2 = r3, 2
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp4.gtu P1, P2 = -119, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   tbit.z P1, P2 = r3, 60
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.ge P1, P2 = 79, r3
// EXPECT p31 = 1
// EXPECT p32 = 1   tbit.nz P1, P2 = r3, 44
// EXPECT p33 = 0
// EXPECT p34 = 0   tbit.z P1, P2 = r3, 44
// EXPECT p35 = 1
// EXPECT p36 = 0   tbit.z P1, P2 = r3, 47
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.eq P1, P2 = 30, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp.le P1, P2 = 85, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp4.lt P1, P2 = -94, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp.leu P1, P2 = -80, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp.ne P1, P2 = 120, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp4.leu P1, P2 = 8, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 1   tbit.z P1, P2 = r3, 2
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x80000000
  movl r3 = 0x4375d0341e4f6f2a
  movl r4 = 0x0
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -93, r3
  and r13 = -93, r3
  or r14 = -93, r3
  xor r15 = -93, r3
  andcm r16 = -93, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -4161, r3
  addl r22 = 730364, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -4161, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 3, 11
  extr r37 = r3, 3, 11
  dep.z r38 = r2, 3, 11
  dep.z r39 = 12, 3, 11
  dep r40 = r2, r3, 3, 7
  dep r41 = -1, r3, 3, 11
  shrp r42 = r2, r3, 51
  shl r43 = r2, 51
  shr.u r44 = r3, 51
  shr r45 = r3, 51
  extr.u r46 = r3, 56, 13
  extr r47 = r3, 56, 13
  dep.z r48 = r2, 56, 13
  dep.z r49 = -71, 56, 13
  dep r50 = r2, r3, 56, 12
  dep r51 = 0, r3, 56, 13
  shrp r52 = r2, r3, 12
  shl r53 = r2, 12
  shr.u r54 = r3, 12
  shr r55 = r3, 12
  extr.u r56 = r3, 62, 25
  extr r57 = r3, 62, 25
  dep.z r58 = r2, 62, 25
  dep.z r59 = 10, 62, 25
  dep r60 = r2, r3, 62, 15
  dep r61 = 0, r3, 62, 25
  shrp r62 = r2, r3, 62
  shl r63 = r2, 62
  shr.u r64 = r3, 62
  shr r65 = r3, 62
  extr.u r66 = r3, 36, 33
  extr r67 = r3, 36, 33
  dep.z r68 = r2, 36, 33
  dep.z r69 = -76, 36, 33
  dep r70 = r2, r3, 36, 4
  dep r71 = 0, r3, 36, 33
  shrp r72 = r2, r3, 35
  shl r73 = r2, 35
  shr.u r74 = r3, 35
  shr r75 = r3, 35
  extr.u r76 = r3, 35, 16
  extr r77 = r3, 35, 16
  dep.z r78 = r2, 35, 16
  dep.z r79 = -114, 35, 16
  dep r80 = r2, r3, 35, 6
  dep r81 = -1, r3, 35, 16
  shrp r82 = r2, r3, 14
  shl r83 = r2, 14
  shr.u r84 = r3, 14
  shr r85 = r3, 14
  extr.u r86 = r3, 11, 55
  extr r87 = r3, 11, 55
  dep.z r88 = r2, 11, 55
  dep.z r89 = 117, 11, 55
  dep r90 = r2, r3, 11, 6
  dep r91 = -1, r3, 11, 55
  shrp r92 = r2, r3, 58
  shl r93 = r2, 58
  shr.u r94 = r3, 58
  shr r95 = r3, 58
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.ge p6, p7 = r2, r3
  cmp4.ne p8, p9 = r2, r3
  cmp.geu p10, p11 = -8, r3
  cmp4.gt p12, p13 = 97, r3
  cmp.gt p14, p15 = r2, r3
  cmp.ne p16, p17 = r2, r3
  tbit.nz p18, p19 = r3, 2
  cmp4.gtu p20, p21 = -119, r3
  cmp4.leu p22, p23 = r2, r3
  tbit.z p24, p25 = r3, 60
  cmp.ltu p26, p27 = r2, r3
  cmp4.lt p28, p29 = r2, r3
  cmp4.ge p30, p31 = 79, r3
  tbit.nz p32, p33 = r3, 44
  tbit.z p34, p35 = r3, 44
  tbit.z p36, p37 = r3, 47
  cmp.eq p38, p39 = 30, r3
  cmp.eq p40, p41 = r2, r3
  cmp.geu p42, p43 = r2, r3
  cmp4.geu p44, p45 = r2, r3
  cmp.le p46, p47 = 85, r3
  cmp4.le p48, p49 = r2, r3
  cmp4.lt p50, p51 = -94, r3
  cmp.leu p52, p53 = -80, r3
  cmp.le p54, p55 = r2, r3
  cmp.ne p56, p57 = 120, r3
  cmp4.leu p58, p59 = 8, r3
  cmp.lt p60, p61 = r2, r3
  tbit.z p62, p63 = r3, 2
  ;;
  break.i 0x1ffffe
