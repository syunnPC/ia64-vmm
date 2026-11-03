/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x100000000   add R = r2, r3
// EXPECT r9 = 0x100000001   add R = r2, r3, 1
// EXPECT r10 = 0x100000000   sub R = r2, r3
// EXPECT r11 = 0xffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffffffffffb0   sub R = -80, r3
// EXPECT r13 = 0x0   and R = -80, r3
// EXPECT r14 = 0xffffffffffffffb0   or R = -80, r3
// EXPECT r15 = 0xffffffffffffffb0   xor R = -80, r3
// EXPECT r16 = 0xffffffffffffffb0   andcm R = -80, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x100000000   or R = r2, r3
// EXPECT r19 = 0x100000000   xor R = r2, r3
// EXPECT r20 = 0x100000000   andcm R = r2, r3
// EXPECT r21 = 0x3a3   adds R = 931, r3
// EXPECT r22 = 0xffffffffffee4dd3   addl R = -1159725, r1
// EXPECT r23 = 0x200000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x400000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x0   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x800000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x1000000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x0   addp4 R = r2, r3
// EXPECT r32 = 0x3a3   addp4 R = 931, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 63, 40
// EXPECT r37 = 0x0   extr R = r3, 63, 40
// EXPECT r38 = 0x0   dep.z R = r2, 63, 40
// EXPECT r39 = 0x0   dep.z R = -62, 63, 40
// EXPECT r40 = 0x0   dep R = r2, r3, 63, 10
// EXPECT r41 = 0x8000000000000000   dep R = -1, r3, 63, 40
// EXPECT r42 = 0x0   shrp R = r2, r3, 32
// EXPECT r43 = 0x0   shl R = r2, 32
// EXPECT r44 = 0x0   shr.u R = r3, 32
// EXPECT r45 = 0x0   shr R = r3, 32
// EXPECT r46 = 0x0   extr.u R = r3, 10, 18
// EXPECT r47 = 0x0   extr R = r3, 10, 18
// EXPECT r48 = 0x0   dep.z R = r2, 10, 18
// EXPECT r49 = 0xffff000   dep.z R = -4, 10, 18
// EXPECT r50 = 0x0   dep R = r2, r3, 10, 11
// EXPECT r51 = 0x0   dep R = 0, r3, 10, 18
// EXPECT r52 = 0x0   shrp R = r2, r3, 3
// EXPECT r53 = 0x800000000   shl R = r2, 3
// EXPECT r54 = 0x0   shr.u R = r3, 3
// EXPECT r55 = 0x0   shr R = r3, 3
// EXPECT r56 = 0x0   extr.u R = r3, 52, 39
// EXPECT r57 = 0x0   extr R = r3, 52, 39
// EXPECT r58 = 0x0   dep.z R = r2, 52, 39
// EXPECT r59 = 0xfdb0000000000000   dep.z R = -37, 52, 39
// EXPECT r60 = 0x0   dep R = r2, r3, 52, 6
// EXPECT r61 = 0xfff0000000000000   dep R = -1, r3, 52, 39
// EXPECT r62 = 0x40000000000000   shrp R = r2, r3, 42
// EXPECT r63 = 0x0   shl R = r2, 42
// EXPECT r64 = 0x0   shr.u R = r3, 42
// EXPECT r65 = 0x0   shr R = r3, 42
// EXPECT r66 = 0x0   extr.u R = r3, 57, 43
// EXPECT r67 = 0x0   extr R = r3, 57, 43
// EXPECT r68 = 0x0   dep.z R = r2, 57, 43
// EXPECT r69 = 0x5800000000000000   dep.z R = 44, 57, 43
// EXPECT r70 = 0x0   dep R = r2, r3, 57, 3
// EXPECT r71 = 0x0   dep R = 0, r3, 57, 43
// EXPECT r72 = 0x0   shrp R = r2, r3, 13
// EXPECT r73 = 0x200000000000   shl R = r2, 13
// EXPECT r74 = 0x0   shr.u R = r3, 13
// EXPECT r75 = 0x0   shr R = r3, 13
// EXPECT r76 = 0x0   extr.u R = r3, 60, 63
// EXPECT r77 = 0x0   extr R = r3, 60, 63
// EXPECT r78 = 0x0   dep.z R = r2, 60, 63
// EXPECT r79 = 0x5000000000000000   dep.z R = 5, 60, 63
// EXPECT r80 = 0x0   dep R = r2, r3, 60, 16
// EXPECT r81 = 0xf000000000000000   dep R = -1, r3, 60, 63
// EXPECT r82 = 0x0   shrp R = r2, r3, 32
// EXPECT r83 = 0x0   shl R = r2, 32
// EXPECT r84 = 0x0   shr.u R = r3, 32
// EXPECT r85 = 0x0   shr R = r3, 32
// EXPECT r86 = 0x0   extr.u R = r3, 21, 24
// EXPECT r87 = 0x0   extr R = r3, 21, 24
// EXPECT r88 = 0x0   dep.z R = r2, 21, 24
// EXPECT r89 = 0x1ffffbe00000   dep.z R = -33, 21, 24
// EXPECT r90 = 0x0   dep R = r2, r3, 21, 10
// EXPECT r91 = 0x1fffffe00000   dep R = -1, r3, 21, 24
// EXPECT r92 = 0x0   shrp R = r2, r3, 28
// EXPECT r93 = 0x1000000000000000   shl R = r2, 28
// EXPECT r94 = 0x0   shr.u R = r3, 28
// EXPECT r95 = 0x0   shr R = r3, 28
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x0   popcnt R = r3
// EXPECT p6 = 0   tbit.nz P1, P2 = r3, 50
// EXPECT p7 = 1
// EXPECT p8 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   cmp.le P1, P2 = 25, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.leu P1, P2 = -118, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp4.geu P1, P2 = -112, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   cmp4.ne P1, P2 = -58, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp4.gt P1, P2 = -32, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.ge P1, P2 = 43, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   tbit.nz P1, P2 = r3, 4
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp4.eq P1, P2 = 30, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.eq P1, P2 = -19, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   cmp.gt P1, P2 = 67, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp.gtu P1, P2 = 26, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   tbit.nz P1, P2 = r3, 56
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 0   cmp4.le P1, P2 = 36, r3
// EXPECT p53 = 1
// EXPECT p54 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p55 = 0
// EXPECT p56 = 1   cmp.gt P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.ltu P1, P2 = 93, r3
// EXPECT p59 = 1
// EXPECT p60 = 1   tbit.z P1, P2 = r3, 4
// EXPECT p61 = 0
// EXPECT p62 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x100000000
  movl r3 = 0x0
  movl r4 = 0xffffffffffffffff
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
  adds r21 = 931, r3
  addl r22 = -1159725, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 931, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 63, 40
  extr r37 = r3, 63, 40
  dep.z r38 = r2, 63, 40
  dep.z r39 = -62, 63, 40
  dep r40 = r2, r3, 63, 10
  dep r41 = -1, r3, 63, 40
  shrp r42 = r2, r3, 32
  shl r43 = r2, 32
  shr.u r44 = r3, 32
  shr r45 = r3, 32
  extr.u r46 = r3, 10, 18
  extr r47 = r3, 10, 18
  dep.z r48 = r2, 10, 18
  dep.z r49 = -4, 10, 18
  dep r50 = r2, r3, 10, 11
  dep r51 = 0, r3, 10, 18
  shrp r52 = r2, r3, 3
  shl r53 = r2, 3
  shr.u r54 = r3, 3
  shr r55 = r3, 3
  extr.u r56 = r3, 52, 39
  extr r57 = r3, 52, 39
  dep.z r58 = r2, 52, 39
  dep.z r59 = -37, 52, 39
  dep r60 = r2, r3, 52, 6
  dep r61 = -1, r3, 52, 39
  shrp r62 = r2, r3, 42
  shl r63 = r2, 42
  shr.u r64 = r3, 42
  shr r65 = r3, 42
  extr.u r66 = r3, 57, 43
  extr r67 = r3, 57, 43
  dep.z r68 = r2, 57, 43
  dep.z r69 = 44, 57, 43
  dep r70 = r2, r3, 57, 3
  dep r71 = 0, r3, 57, 43
  shrp r72 = r2, r3, 13
  shl r73 = r2, 13
  shr.u r74 = r3, 13
  shr r75 = r3, 13
  extr.u r76 = r3, 60, 63
  extr r77 = r3, 60, 63
  dep.z r78 = r2, 60, 63
  dep.z r79 = 5, 60, 63
  dep r80 = r2, r3, 60, 16
  dep r81 = -1, r3, 60, 63
  shrp r82 = r2, r3, 32
  shl r83 = r2, 32
  shr.u r84 = r3, 32
  shr r85 = r3, 32
  extr.u r86 = r3, 21, 24
  extr r87 = r3, 21, 24
  dep.z r88 = r2, 21, 24
  dep.z r89 = -33, 21, 24
  dep r90 = r2, r3, 21, 10
  dep r91 = -1, r3, 21, 24
  shrp r92 = r2, r3, 28
  shl r93 = r2, 28
  shr.u r94 = r3, 28
  shr r95 = r3, 28
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  tbit.nz p6, p7 = r3, 50
  cmp.eq p8, p9 = r2, r3
  cmp.le p10, p11 = 25, r3
  cmp4.geu p12, p13 = r2, r3
  cmp4.leu p14, p15 = -118, r3
  cmp4.geu p16, p17 = -112, r3
  cmp4.ne p18, p19 = -58, r3
  cmp.ge p20, p21 = r2, r3
  cmp4.le p22, p23 = r2, r3
  cmp4.gt p24, p25 = -32, r3
  cmp.le p26, p27 = r2, r3
  cmp4.ge p28, p29 = 43, r3
  cmp4.lt p30, p31 = r2, r3
  tbit.nz p32, p33 = r3, 4
  cmp.lt p34, p35 = r2, r3
  cmp4.eq p36, p37 = 30, r3
  cmp.eq p38, p39 = -19, r3
  cmp.ltu p40, p41 = r2, r3
  cmp.gt p42, p43 = 67, r3
  cmp.geu p44, p45 = r2, r3
  cmp.gtu p46, p47 = 26, r3
  tbit.nz p48, p49 = r3, 56
  cmp4.gtu p50, p51 = r2, r3
  cmp4.le p52, p53 = 36, r3
  cmp.ne p54, p55 = r2, r3
  cmp.gt p56, p57 = r2, r3
  cmp.ltu p58, p59 = 93, r3
  tbit.z p60, p61 = r3, 4
  cmp4.eq p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
