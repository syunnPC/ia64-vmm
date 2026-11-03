/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xe1901a305987f0c0   add R = r2, r3
// EXPECT r9 = 0xe1901a305987f0c1   add R = r2, r3, 1
// EXPECT r10 = 0x314a543e2c6b4fb2   sub R = r2, r3
// EXPECT r11 = 0x314a543e2c6b4fb1   sub R = r2, r3, 1
// EXPECT r12 = 0x27dd1d06e971af5f   sub R = -26, r3
// EXPECT r13 = 0xd822e2f9168e5086   and R = -26, r3
// EXPECT r14 = 0xffffffffffffffe7   or R = -26, r3
// EXPECT r15 = 0x27dd1d06e971af61   xor R = -26, r3
// EXPECT r16 = 0x27dd1d06e971af60   andcm R = -26, r3
// EXPECT r17 = 0x820223102880001   and R = r2, r3
// EXPECT r18 = 0xd96ff7ff56fff0bf   or R = r2, r3
// EXPECT r19 = 0xd14fd5ce5477f0be   xor R = r2, r3
// EXPECT r20 = 0x14d15064071a038   andcm R = r2, r3
// EXPECT r21 = 0xd822e2f9168e5ea7   adds R = 3616, r3
// EXPECT r22 = 0x1e4c40   addl R = 1985600, r1
// EXPECT r23 = 0xeafd51679c8190f9   shladd R = r2, 1, r3
// EXPECT r24 = 0x9c8190f9   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfdd7bfd62274d16b   shladd R = r2, 2, r3
// EXPECT r26 = 0x2274d16b   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x238c9cb32e5b524f   shladd R = r2, 3, r3
// EXPECT r28 = 0x2e5b524f   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x6ef6566d46285417   shladd R = r2, 4, r3
// EXPECT r30 = 0x46285417   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x5987f0c0   addp4 R = r2, r3
// EXPECT r32 = 0x168e5ea7   addp4 R = 3616, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x6   extr.u R = r3, 24, 4
// EXPECT r37 = 0x6   extr R = r3, 24, 4
// EXPECT r38 = 0x9000000   dep.z R = r2, 24, 4
// EXPECT r39 = 0x9000000   dep.z R = 57, 24, 4
// EXPECT r40 = 0xd822e2f9398e5087   dep R = r2, r3, 24, 8
// EXPECT r41 = 0xd822e2f91f8e5087   dep R = -1, r3, 24, 4
// EXPECT r42 = 0xdd0be680e7608b8b   shrp R = r2, r3, 38
// EXPECT r43 = 0xbe680e4000000000   shl R = r2, 38
// EXPECT r44 = 0x3608b8b   shr.u R = r3, 38
// EXPECT r45 = 0xffffffffff608b8b   shr R = r3, 38
// EXPECT r46 = 0xc117   extr.u R = r3, 45, 16
// EXPECT r47 = 0xffffffffffffc117   extr R = r3, 45, 16
// EXPECT r48 = 0x1407200000000000   dep.z R = r2, 45, 16
// EXPECT r49 = 0x1ff5a00000000000   dep.z R = -83, 45, 16
// EXPECT r50 = 0xd82722f9168e5087   dep R = r2, r3, 45, 7
// EXPECT r51 = 0xc00002f9168e5087   dep R = 0, r3, 45, 16
// EXPECT r52 = 0xd0be680e7608b8be   shrp R = r2, r3, 34
// EXPECT r53 = 0xbe680e400000000   shl R = r2, 34
// EXPECT r54 = 0x3608b8be   shr.u R = r3, 34
// EXPECT r55 = 0xfffffffff608b8be   shr R = r3, 34
// EXPECT r56 = 0x1b045c5   extr.u R = r3, 39, 26
// EXPECT r57 = 0xffffffffffb045c5   extr R = r3, 39, 26
// EXPECT r58 = 0x7cd01c8000000000   dep.z R = r2, 39, 26
// EXPECT r59 = 0x210000000000   dep.z R = 66, 39, 26
// EXPECT r60 = 0xd8501cf9168e5087   dep R = r2, r3, 39, 16
// EXPECT r61 = 0x79168e5087   dep R = 0, r3, 39, 26
// EXPECT r62 = 0xd01cec11717c8b47   shrp R = r2, r3, 17
// EXPECT r63 = 0x6e6e85f340720000   shl R = r2, 17
// EXPECT r64 = 0x6c11717c8b47   shr.u R = r3, 17
// EXPECT r65 = 0xffffec11717c8b47   shr R = r3, 17
// EXPECT r66 = 0x1   extr.u R = r3, 26, 2
// EXPECT r67 = 0x1   extr R = r3, 26, 2
// EXPECT r68 = 0x4000000   dep.z R = r2, 26, 2
// EXPECT r69 = 0x0   dep.z R = -32, 26, 2
// EXPECT r70 = 0xd822e2f9e68e5087   dep R = r2, r3, 26, 6
// EXPECT r71 = 0xd822e2f9128e5087   dep R = 0, r3, 26, 2
// EXPECT r72 = 0xcdd0be680e7608b8   shrp R = r2, r3, 42
// EXPECT r73 = 0xe680e40000000000   shl R = r2, 42
// EXPECT r74 = 0x3608b8   shr.u R = r3, 42
// EXPECT r75 = 0xfffffffffff608b8   shr R = r3, 42
// EXPECT r76 = 0x1b045c5   extr.u R = r3, 39, 48
// EXPECT r77 = 0xffffffffffb045c5   extr R = r3, 39, 48
// EXPECT r78 = 0x7cd01c8000000000   dep.z R = r2, 39, 48
// EXPECT r79 = 0x208000000000   dep.z R = 65, 39, 48
// EXPECT r80 = 0xd8201cf9168e5087   dep R = r2, r3, 39, 13
// EXPECT r81 = 0xfffffff9168e5087   dep R = -1, r3, 39, 48
// EXPECT r82 = 0xa039d822e2f9168e   shrp R = r2, r3, 16
// EXPECT r83 = 0x373742f9a0390000   shl R = r2, 16
// EXPECT r84 = 0xd822e2f9168e   shr.u R = r3, 16
// EXPECT r85 = 0xffffd822e2f9168e   shr R = r3, 16
// EXPECT r86 = 0x3   extr.u R = r3, 62, 7
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 62, 7
// EXPECT r88 = 0x4000000000000000   dep.z R = r2, 62, 7
// EXPECT r89 = 0x8000000000000000   dep.z R = -34, 62, 7
// EXPECT r90 = 0x5822e2f9168e5087   dep R = r2, r3, 62, 14
// EXPECT r91 = 0xd822e2f9168e5087   dep R = -1, r3, 62, 7
// EXPECT r92 = 0x39d822e2f9168e5   shrp R = r2, r3, 12
// EXPECT r93 = 0xd373742f9a039000   shl R = r2, 12
// EXPECT r94 = 0xd822e2f9168e5   shr.u R = r3, 12
// EXPECT r95 = 0xfffd822e2f9168e5   shr R = r3, 12
// EXPECT r96 = 0xffffffffffffff87   sxt1 R = r3
// EXPECT r97 = 0x5087   sxt2 R = r3
// EXPECT r98 = 0x168e5087   sxt4 R = r3
// EXPECT r99 = 0x87   zxt1 R = r3
// EXPECT r100 = 0x5087   zxt2 R = r3
// EXPECT r101 = 0x168e5087   zxt4 R = r3
// EXPECT r102 = 0x1d   popcnt R = r3
// EXPECT p6 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp4.eq P1, P2 = 81, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp4.gt P1, P2 = -40, r3
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   tbit.nz P1, P2 = r3, 46
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp4.geu P1, P2 = -65, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp4.leu P1, P2 = 121, r3
// EXPECT p23 = 0
// EXPECT p24 = 1   cmp.ge P1, P2 = 113, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp4.ge P1, P2 = -117, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   tbit.nz P1, P2 = r3, 32
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp.le P1, P2 = 100, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   cmp4.gtu P1, P2 = -119, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.geu P1, P2 = 27, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp4.lt P1, P2 = -30, r3
// EXPECT p43 = 0
// EXPECT p44 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   cmp.leu P1, P2 = 66, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 24
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp.lt P1, P2 = -83, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.z P1, P2 = r3, 32
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp4.ne P1, P2 = -85, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.ltu P1, P2 = 8, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x96d373742f9a039
  movl r3 = 0xd822e2f9168e5087
  movl r4 = 0xffffffffffffffff
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -26, r3
  and r13 = -26, r3
  or r14 = -26, r3
  xor r15 = -26, r3
  andcm r16 = -26, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 3616, r3
  addl r22 = 1985600, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 3616, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 24, 4
  extr r37 = r3, 24, 4
  dep.z r38 = r2, 24, 4
  dep.z r39 = 57, 24, 4
  dep r40 = r2, r3, 24, 8
  dep r41 = -1, r3, 24, 4
  shrp r42 = r2, r3, 38
  shl r43 = r2, 38
  shr.u r44 = r3, 38
  shr r45 = r3, 38
  extr.u r46 = r3, 45, 16
  extr r47 = r3, 45, 16
  dep.z r48 = r2, 45, 16
  dep.z r49 = -83, 45, 16
  dep r50 = r2, r3, 45, 7
  dep r51 = 0, r3, 45, 16
  shrp r52 = r2, r3, 34
  shl r53 = r2, 34
  shr.u r54 = r3, 34
  shr r55 = r3, 34
  extr.u r56 = r3, 39, 26
  extr r57 = r3, 39, 26
  dep.z r58 = r2, 39, 26
  dep.z r59 = 66, 39, 26
  dep r60 = r2, r3, 39, 16
  dep r61 = 0, r3, 39, 26
  shrp r62 = r2, r3, 17
  shl r63 = r2, 17
  shr.u r64 = r3, 17
  shr r65 = r3, 17
  extr.u r66 = r3, 26, 2
  extr r67 = r3, 26, 2
  dep.z r68 = r2, 26, 2
  dep.z r69 = -32, 26, 2
  dep r70 = r2, r3, 26, 6
  dep r71 = 0, r3, 26, 2
  shrp r72 = r2, r3, 42
  shl r73 = r2, 42
  shr.u r74 = r3, 42
  shr r75 = r3, 42
  extr.u r76 = r3, 39, 48
  extr r77 = r3, 39, 48
  dep.z r78 = r2, 39, 48
  dep.z r79 = 65, 39, 48
  dep r80 = r2, r3, 39, 13
  dep r81 = -1, r3, 39, 48
  shrp r82 = r2, r3, 16
  shl r83 = r2, 16
  shr.u r84 = r3, 16
  shr r85 = r3, 16
  extr.u r86 = r3, 62, 7
  extr r87 = r3, 62, 7
  dep.z r88 = r2, 62, 7
  dep.z r89 = -34, 62, 7
  dep r90 = r2, r3, 62, 14
  dep r91 = -1, r3, 62, 7
  shrp r92 = r2, r3, 12
  shl r93 = r2, 12
  shr.u r94 = r3, 12
  shr r95 = r3, 12
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = r2, r3
  cmp4.le p8, p9 = r2, r3
  cmp4.eq p10, p11 = 81, r3
  cmp4.gt p12, p13 = -40, r3
  cmp.ge p14, p15 = r2, r3
  cmp4.ltu p16, p17 = r2, r3
  tbit.nz p18, p19 = r3, 46
  cmp4.geu p20, p21 = -65, r3
  cmp4.leu p22, p23 = 121, r3
  cmp.ge p24, p25 = 113, r3
  cmp4.leu p26, p27 = r2, r3
  cmp4.ge p28, p29 = -117, r3
  tbit.nz p30, p31 = r3, 32
  cmp.le p32, p33 = 100, r3
  cmp4.lt p34, p35 = r2, r3
  cmp4.gtu p36, p37 = -119, r3
  cmp.geu p38, p39 = 27, r3
  cmp4.eq p40, p41 = r2, r3
  cmp4.lt p42, p43 = -30, r3
  cmp.gtu p44, p45 = r2, r3
  cmp.leu p46, p47 = r2, r3
  cmp.leu p48, p49 = 66, r3
  tbit.z p50, p51 = r3, 24
  cmp.lt p52, p53 = -83, r3
  tbit.z p54, p55 = r3, 32
  cmp.gt p56, p57 = r2, r3
  cmp4.ne p58, p59 = -85, r3
  cmp.le p60, p61 = r2, r3
  cmp4.ltu p62, p63 = 8, r3
  ;;
  break.i 0x1ffffe
