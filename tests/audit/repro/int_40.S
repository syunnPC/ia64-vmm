/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x1   add R = r2, r3
// EXPECT r9 = 0x2   add R = r2, r3, 1
// EXPECT r10 = 0xfffffffffffffffd   sub R = r2, r3
// EXPECT r11 = 0xfffffffffffffffc   sub R = r2, r3, 1
// EXPECT r12 = 0x29   sub R = 43, r3
// EXPECT r13 = 0x2   and R = 43, r3
// EXPECT r14 = 0x2b   or R = 43, r3
// EXPECT r15 = 0x29   xor R = 43, r3
// EXPECT r16 = 0x29   andcm R = 43, r3
// EXPECT r17 = 0x2   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xfffffffffffffffd   xor R = r2, r3
// EXPECT r20 = 0xfffffffffffffffd   andcm R = r2, r3
// EXPECT r21 = 0xfffffffffffffd12   adds R = -752, r3
// EXPECT r22 = 0xfffffffffff3b06f   addl R = -806801, r1
// EXPECT r23 = 0x0   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfffffffffffffffe   shladd R = r2, 2, r3
// EXPECT r26 = 0xfffffffe   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xfffffffffffffffa   shladd R = r2, 3, r3
// EXPECT r28 = 0xfffffffa   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xfffffffffffffff2   shladd R = r2, 4, r3
// EXPECT r30 = 0xfffffff2   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x1   addp4 R = r2, r3
// EXPECT r32 = 0xfffffd12   addp4 R = -752, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 39, 21
// EXPECT r37 = 0x0   extr R = r3, 39, 21
// EXPECT r38 = 0xfffff8000000000   dep.z R = r2, 39, 21
// EXPECT r39 = 0xf0000000000   dep.z R = 30, 39, 21
// EXPECT r40 = 0x1ff8000000002   dep R = r2, r3, 39, 10
// EXPECT r41 = 0xfffff8000000002   dep R = -1, r3, 39, 21
// EXPECT r42 = 0xfffff00000000000   shrp R = r2, r3, 20
// EXPECT r43 = 0xfffffffffff00000   shl R = r2, 20
// EXPECT r44 = 0x0   shr.u R = r3, 20
// EXPECT r45 = 0x0   shr R = r3, 20
// EXPECT r46 = 0x0   extr.u R = r3, 26, 63
// EXPECT r47 = 0x0   extr R = r3, 26, 63
// EXPECT r48 = 0xfffffffffc000000   dep.z R = r2, 26, 63
// EXPECT r49 = 0x174000000   dep.z R = 93, 26, 63
// EXPECT r50 = 0x1c000002   dep R = r2, r3, 26, 3
// EXPECT r51 = 0x2   dep R = 0, r3, 26, 63
// EXPECT r52 = 0xffffffc000000000   shrp R = r2, r3, 26
// EXPECT r53 = 0xfffffffffc000000   shl R = r2, 26
// EXPECT r54 = 0x0   shr.u R = r3, 26
// EXPECT r55 = 0x0   shr R = r3, 26
// EXPECT r56 = 0x1   extr.u R = r3, 1, 12
// EXPECT r57 = 0x1   extr R = r3, 1, 12
// EXPECT r58 = 0x1ffe   dep.z R = r2, 1, 12
// EXPECT r59 = 0xde   dep.z R = 111, 1, 12
// EXPECT r60 = 0x7fe   dep R = r2, r3, 1, 10
// EXPECT r61 = 0x0   dep R = 0, r3, 1, 12
// EXPECT r62 = 0xffffff0000000000   shrp R = r2, r3, 24
// EXPECT r63 = 0xffffffffff000000   shl R = r2, 24
// EXPECT r64 = 0x0   shr.u R = r3, 24
// EXPECT r65 = 0x0   shr R = r3, 24
// EXPECT r66 = 0x0   extr.u R = r3, 30, 60
// EXPECT r67 = 0x0   extr R = r3, 30, 60
// EXPECT r68 = 0xffffffffc0000000   dep.z R = r2, 30, 60
// EXPECT r69 = 0x1a00000000   dep.z R = 104, 30, 60
// EXPECT r70 = 0x3c0000002   dep R = r2, r3, 30, 4
// EXPECT r71 = 0xffffffffc0000002   dep R = -1, r3, 30, 60
// EXPECT r72 = 0xffffffffffc00000   shrp R = r2, r3, 42
// EXPECT r73 = 0xfffffc0000000000   shl R = r2, 42
// EXPECT r74 = 0x0   shr.u R = r3, 42
// EXPECT r75 = 0x0   shr R = r3, 42
// EXPECT r76 = 0x0   extr.u R = r3, 58, 37
// EXPECT r77 = 0x0   extr R = r3, 58, 37
// EXPECT r78 = 0xfc00000000000000   dep.z R = r2, 58, 37
// EXPECT r79 = 0xf800000000000000   dep.z R = -2, 58, 37
// EXPECT r80 = 0x1c00000000000002   dep R = r2, r3, 58, 3
// EXPECT r81 = 0x2   dep R = 0, r3, 58, 37
// EXPECT r82 = 0xffffe00000000000   shrp R = r2, r3, 19
// EXPECT r83 = 0xfffffffffff80000   shl R = r2, 19
// EXPECT r84 = 0x0   shr.u R = r3, 19
// EXPECT r85 = 0x0   shr R = r3, 19
// EXPECT r86 = 0x0   extr.u R = r3, 49, 24
// EXPECT r87 = 0x0   extr R = r3, 49, 24
// EXPECT r88 = 0xfffe000000000000   dep.z R = r2, 49, 24
// EXPECT r89 = 0xc8000000000000   dep.z R = 100, 49, 24
// EXPECT r90 = 0x1e000000000002   dep R = r2, r3, 49, 4
// EXPECT r91 = 0xfffe000000000002   dep R = -1, r3, 49, 24
// EXPECT r92 = 0xffffff0000000000   shrp R = r2, r3, 24
// EXPECT r93 = 0xffffffffff000000   shl R = r2, 24
// EXPECT r94 = 0x0   shr.u R = r3, 24
// EXPECT r95 = 0x0   shr R = r3, 24
// EXPECT r96 = 0x2   sxt1 R = r3
// EXPECT r97 = 0x2   sxt2 R = r3
// EXPECT r98 = 0x2   sxt4 R = r3
// EXPECT r99 = 0x2   zxt1 R = r3
// EXPECT r100 = 0x2   zxt2 R = r3
// EXPECT r101 = 0x2   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp.geu P1, P2 = 28, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   tbit.nz P1, P2 = r3, 55
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p13 = 1
// EXPECT p14 = 1   cmp.gtu P1, P2 = -41, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   tbit.nz P1, P2 = r3, 30
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp4.lt P1, P2 = -40, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   tbit.nz P1, P2 = r3, 27
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   tbit.z P1, P2 = r3, 53
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp.eq P1, P2 = -119, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp4.ne P1, P2 = -82, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   tbit.z P1, P2 = r3, 30
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp4.ltu P1, P2 = 44, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp4.ge P1, P2 = -33, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp4.gt P1, P2 = -99, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp4.gtu P1, P2 = -5, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.ge P1, P2 = -47, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 1   cmp4.le P1, P2 = -45, r3
// EXPECT p53 = 0
// EXPECT p54 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   tbit.z P1, P2 = r3, 27
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp4.ltu P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0xffffffffffffffff
  movl r3 = 0x2
  movl r4 = 0x40
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 43, r3
  and r13 = 43, r3
  or r14 = 43, r3
  xor r15 = 43, r3
  andcm r16 = 43, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -752, r3
  addl r22 = -806801, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -752, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 39, 21
  extr r37 = r3, 39, 21
  dep.z r38 = r2, 39, 21
  dep.z r39 = 30, 39, 21
  dep r40 = r2, r3, 39, 10
  dep r41 = -1, r3, 39, 21
  shrp r42 = r2, r3, 20
  shl r43 = r2, 20
  shr.u r44 = r3, 20
  shr r45 = r3, 20
  extr.u r46 = r3, 26, 63
  extr r47 = r3, 26, 63
  dep.z r48 = r2, 26, 63
  dep.z r49 = 93, 26, 63
  dep r50 = r2, r3, 26, 3
  dep r51 = 0, r3, 26, 63
  shrp r52 = r2, r3, 26
  shl r53 = r2, 26
  shr.u r54 = r3, 26
  shr r55 = r3, 26
  extr.u r56 = r3, 1, 12
  extr r57 = r3, 1, 12
  dep.z r58 = r2, 1, 12
  dep.z r59 = 111, 1, 12
  dep r60 = r2, r3, 1, 10
  dep r61 = 0, r3, 1, 12
  shrp r62 = r2, r3, 24
  shl r63 = r2, 24
  shr.u r64 = r3, 24
  shr r65 = r3, 24
  extr.u r66 = r3, 30, 60
  extr r67 = r3, 30, 60
  dep.z r68 = r2, 30, 60
  dep.z r69 = 104, 30, 60
  dep r70 = r2, r3, 30, 4
  dep r71 = -1, r3, 30, 60
  shrp r72 = r2, r3, 42
  shl r73 = r2, 42
  shr.u r74 = r3, 42
  shr r75 = r3, 42
  extr.u r76 = r3, 58, 37
  extr r77 = r3, 58, 37
  dep.z r78 = r2, 58, 37
  dep.z r79 = -2, 58, 37
  dep r80 = r2, r3, 58, 3
  dep r81 = 0, r3, 58, 37
  shrp r82 = r2, r3, 19
  shl r83 = r2, 19
  shr.u r84 = r3, 19
  shr r85 = r3, 19
  extr.u r86 = r3, 49, 24
  extr r87 = r3, 49, 24
  dep.z r88 = r2, 49, 24
  dep.z r89 = 100, 49, 24
  dep r90 = r2, r3, 49, 4
  dep r91 = -1, r3, 49, 24
  shrp r92 = r2, r3, 24
  shl r93 = r2, 24
  shr.u r94 = r3, 24
  shr r95 = r3, 24
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.leu p6, p7 = r2, r3
  cmp.geu p8, p9 = 28, r3
  tbit.nz p10, p11 = r3, 55
  cmp4.gt p12, p13 = r2, r3
  cmp.gtu p14, p15 = -41, r3
  cmp.ne p16, p17 = r2, r3
  tbit.nz p18, p19 = r3, 30
  cmp4.lt p20, p21 = -40, r3
  tbit.nz p22, p23 = r3, 27
  cmp.leu p24, p25 = r2, r3
  cmp4.geu p26, p27 = r2, r3
  tbit.z p28, p29 = r3, 53
  cmp.eq p30, p31 = -119, r3
  cmp4.ge p32, p33 = r2, r3
  cmp4.ne p34, p35 = -82, r3
  tbit.z p36, p37 = r3, 30
  cmp4.ltu p38, p39 = 44, r3
  cmp4.ge p40, p41 = -33, r3
  cmp.ge p42, p43 = r2, r3
  cmp4.gt p44, p45 = -99, r3
  cmp4.gtu p46, p47 = -5, r3
  cmp.ge p48, p49 = -47, r3
  cmp4.gtu p50, p51 = r2, r3
  cmp4.le p52, p53 = -45, r3
  cmp.le p54, p55 = r2, r3
  cmp.geu p56, p57 = r2, r3
  tbit.z p58, p59 = r3, 27
  cmp4.le p60, p61 = r2, r3
  cmp4.ltu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
