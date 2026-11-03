/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xfffffffffffffffe   add R = r2, r3
// EXPECT r9 = 0xffffffffffffffff   add R = r2, r3, 1
// EXPECT r10 = 0xfffffffffffffffe   sub R = r2, r3
// EXPECT r11 = 0xfffffffffffffffd   sub R = r2, r3, 1
// EXPECT r12 = 0x58   sub R = 88, r3
// EXPECT r13 = 0x0   and R = 88, r3
// EXPECT r14 = 0x58   or R = 88, r3
// EXPECT r15 = 0x58   xor R = 88, r3
// EXPECT r16 = 0x58   andcm R = 88, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0xfffffffffffffffe   or R = r2, r3
// EXPECT r19 = 0xfffffffffffffffe   xor R = r2, r3
// EXPECT r20 = 0xfffffffffffffffe   andcm R = r2, r3
// EXPECT r21 = 0x12b2   adds R = 4786, r3
// EXPECT r22 = 0x185bcb   addl R = 1596363, r1
// EXPECT r23 = 0xfffffffffffffffc   shladd R = r2, 1, r3
// EXPECT r24 = 0xfffffffc   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xfffffffffffffff8   shladd R = r2, 2, r3
// EXPECT r26 = 0xfffffff8   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xfffffffffffffff0   shladd R = r2, 3, r3
// EXPECT r28 = 0xfffffff0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xffffffffffffffe0   shladd R = r2, 4, r3
// EXPECT r30 = 0xffffffe0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0xfffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x12b2   addp4 R = 4786, r3
// EXPECT r33 = 0xfffffffffffffffc   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 38, 21
// EXPECT r37 = 0x0   extr R = r3, 38, 21
// EXPECT r38 = 0x7ffff8000000000   dep.z R = r2, 38, 21
// EXPECT r39 = 0x140000000000   dep.z R = 80, 38, 21
// EXPECT r40 = 0x3f8000000000   dep R = r2, r3, 38, 8
// EXPECT r41 = 0x0   dep R = 0, r3, 38, 21
// EXPECT r42 = 0xffffff8000000000   shrp R = r2, r3, 26
// EXPECT r43 = 0xfffffffff8000000   shl R = r2, 26
// EXPECT r44 = 0x0   shr.u R = r3, 26
// EXPECT r45 = 0x0   shr R = r3, 26
// EXPECT r46 = 0x0   extr.u R = r3, 49, 25
// EXPECT r47 = 0x0   extr R = r3, 49, 25
// EXPECT r48 = 0xfffc000000000000   dep.z R = r2, 49, 25
// EXPECT r49 = 0xff8c000000000000   dep.z R = -58, 49, 25
// EXPECT r50 = 0x3c000000000000   dep R = r2, r3, 49, 5
// EXPECT r51 = 0xfffe000000000000   dep R = -1, r3, 49, 25
// EXPECT r52 = 0xfff0000000000000   shrp R = r2, r3, 13
// EXPECT r53 = 0xffffffffffffc000   shl R = r2, 13
// EXPECT r54 = 0x0   shr.u R = r3, 13
// EXPECT r55 = 0x0   shr R = r3, 13
// EXPECT r56 = 0x0   extr.u R = r3, 58, 1
// EXPECT r57 = 0x0   extr R = r3, 58, 1
// EXPECT r58 = 0x0   dep.z R = r2, 58, 1
// EXPECT r59 = 0x0   dep.z R = 104, 58, 1
// EXPECT r60 = 0xf800000000000000   dep R = r2, r3, 58, 16
// EXPECT r61 = 0x0   dep R = 0, r3, 58, 1
// EXPECT r62 = 0xfffffffffffffff0   shrp R = r2, r3, 61
// EXPECT r63 = 0xc000000000000000   shl R = r2, 61
// EXPECT r64 = 0x0   shr.u R = r3, 61
// EXPECT r65 = 0x0   shr R = r3, 61
// EXPECT r66 = 0x0   extr.u R = r3, 63, 48
// EXPECT r67 = 0x0   extr R = r3, 63, 48
// EXPECT r68 = 0x0   dep.z R = r2, 63, 48
// EXPECT r69 = 0x8000000000000000   dep.z R = 91, 63, 48
// EXPECT r70 = 0x0   dep R = r2, r3, 63, 6
// EXPECT r71 = 0x0   dep R = 0, r3, 63, 48
// EXPECT r72 = 0xfffffffc00000000   shrp R = r2, r3, 31
// EXPECT r73 = 0xffffffff00000000   shl R = r2, 31
// EXPECT r74 = 0x0   shr.u R = r3, 31
// EXPECT r75 = 0x0   shr R = r3, 31
// EXPECT r76 = 0x0   extr.u R = r3, 11, 4
// EXPECT r77 = 0x0   extr R = r3, 11, 4
// EXPECT r78 = 0x7000   dep.z R = r2, 11, 4
// EXPECT r79 = 0x7000   dep.z R = 110, 11, 4
// EXPECT r80 = 0xfff000   dep R = r2, r3, 11, 13
// EXPECT r81 = 0x7800   dep R = -1, r3, 11, 4
// EXPECT r82 = 0xfff8000000000000   shrp R = r2, r3, 14
// EXPECT r83 = 0xffffffffffff8000   shl R = r2, 14
// EXPECT r84 = 0x0   shr.u R = r3, 14
// EXPECT r85 = 0x0   shr R = r3, 14
// EXPECT r86 = 0x0   extr.u R = r3, 33, 17
// EXPECT r87 = 0x0   extr R = r3, 33, 17
// EXPECT r88 = 0x3fffc00000000   dep.z R = r2, 33, 17
// EXPECT r89 = 0xe600000000   dep.z R = 115, 33, 17
// EXPECT r90 = 0x7fc00000000   dep R = r2, r3, 33, 10
// EXPECT r91 = 0x3fffe00000000   dep R = -1, r3, 33, 17
// EXPECT r92 = 0xffffffffffffc000   shrp R = r2, r3, 51
// EXPECT r93 = 0xfff0000000000000   shl R = r2, 51
// EXPECT r94 = 0x0   shr.u R = r3, 51
// EXPECT r95 = 0x0   shr R = r3, 51
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x0   popcnt R = r3
// EXPECT p6 = 0   cmp4.ltu P1, P2 = -94, r3
// EXPECT p7 = 1
// EXPECT p8 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp4.le P1, P2 = 111, r3
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp.ltu P1, P2 = 96, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p15 = 1
// EXPECT p16 = 1   cmp4.geu P1, P2 = 12, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   cmp4.leu P1, P2 = r2, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 1   tbit.z P1, P2 = r3, 21
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.le P1, P2 = 26, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   tbit.nz P1, P2 = r3, 21
// EXPECT p31 = 1
// EXPECT p32 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.gt P1, P2 = -105, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 0   cmp4.gt P1, P2 = r2, r3
// EXPECT p43 = 1
// EXPECT p44 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p45 = 1
// EXPECT p46 = 0   cmp.eq P1, P2 = 22, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.ge P1, P2 = 95, r3
// EXPECT p49 = 0
// EXPECT p50 = 0   cmp4.ge P1, P2 = r2, r3
// EXPECT p51 = 1
// EXPECT p52 = 1   tbit.z P1, P2 = r3, 9
// EXPECT p53 = 0
// EXPECT p54 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp.ltu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp4.gtu P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp4.gt P1, P2 = -120, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.geu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0xfffffffffffffffe
  movl r3 = 0x0
  movl r4 = 0x1
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 88, r3
  and r13 = 88, r3
  or r14 = 88, r3
  xor r15 = 88, r3
  andcm r16 = 88, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 4786, r3
  addl r22 = 1596363, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 4786, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 38, 21
  extr r37 = r3, 38, 21
  dep.z r38 = r2, 38, 21
  dep.z r39 = 80, 38, 21
  dep r40 = r2, r3, 38, 8
  dep r41 = 0, r3, 38, 21
  shrp r42 = r2, r3, 26
  shl r43 = r2, 26
  shr.u r44 = r3, 26
  shr r45 = r3, 26
  extr.u r46 = r3, 49, 25
  extr r47 = r3, 49, 25
  dep.z r48 = r2, 49, 25
  dep.z r49 = -58, 49, 25
  dep r50 = r2, r3, 49, 5
  dep r51 = -1, r3, 49, 25
  shrp r52 = r2, r3, 13
  shl r53 = r2, 13
  shr.u r54 = r3, 13
  shr r55 = r3, 13
  extr.u r56 = r3, 58, 1
  extr r57 = r3, 58, 1
  dep.z r58 = r2, 58, 1
  dep.z r59 = 104, 58, 1
  dep r60 = r2, r3, 58, 16
  dep r61 = 0, r3, 58, 1
  shrp r62 = r2, r3, 61
  shl r63 = r2, 61
  shr.u r64 = r3, 61
  shr r65 = r3, 61
  extr.u r66 = r3, 63, 48
  extr r67 = r3, 63, 48
  dep.z r68 = r2, 63, 48
  dep.z r69 = 91, 63, 48
  dep r70 = r2, r3, 63, 6
  dep r71 = 0, r3, 63, 48
  shrp r72 = r2, r3, 31
  shl r73 = r2, 31
  shr.u r74 = r3, 31
  shr r75 = r3, 31
  extr.u r76 = r3, 11, 4
  extr r77 = r3, 11, 4
  dep.z r78 = r2, 11, 4
  dep.z r79 = 110, 11, 4
  dep r80 = r2, r3, 11, 13
  dep r81 = -1, r3, 11, 4
  shrp r82 = r2, r3, 14
  shl r83 = r2, 14
  shr.u r84 = r3, 14
  shr r85 = r3, 14
  extr.u r86 = r3, 33, 17
  extr r87 = r3, 33, 17
  dep.z r88 = r2, 33, 17
  dep.z r89 = 115, 33, 17
  dep r90 = r2, r3, 33, 10
  dep r91 = -1, r3, 33, 17
  shrp r92 = r2, r3, 51
  shl r93 = r2, 51
  shr.u r94 = r3, 51
  shr r95 = r3, 51
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.ltu p6, p7 = -94, r3
  cmp.geu p8, p9 = r2, r3
  cmp4.le p10, p11 = 111, r3
  cmp.ltu p12, p13 = 96, r3
  cmp.gt p14, p15 = r2, r3
  cmp4.geu p16, p17 = 12, r3
  cmp4.leu p18, p19 = r2, r3
  cmp.lt p20, p21 = r2, r3
  tbit.z p22, p23 = r3, 21
  cmp.le p24, p25 = 26, r3
  cmp.eq p26, p27 = r2, r3
  cmp4.lt p28, p29 = r2, r3
  tbit.nz p30, p31 = r3, 21
  cmp.ne p32, p33 = r2, r3
  cmp4.ne p34, p35 = r2, r3
  cmp.ge p36, p37 = r2, r3
  cmp.gt p38, p39 = -105, r3
  cmp.le p40, p41 = r2, r3
  cmp4.gt p42, p43 = r2, r3
  cmp.leu p44, p45 = r2, r3
  cmp.eq p46, p47 = 22, r3
  cmp4.ge p48, p49 = 95, r3
  cmp4.ge p50, p51 = r2, r3
  tbit.z p52, p53 = r3, 9
  cmp4.eq p54, p55 = r2, r3
  cmp.ltu p56, p57 = r2, r3
  cmp4.gtu p58, p59 = r2, r3
  cmp4.gt p60, p61 = -120, r3
  cmp4.geu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
