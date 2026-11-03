/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x8000000080000000   add R = r2, r3
// EXPECT r9 = 0x8000000080000001   add R = r2, r3, 1
// EXPECT r10 = 0x7fffffff80000000   sub R = r2, r3
// EXPECT r11 = 0x7fffffff7fffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff7fffffad   sub R = -83, r3
// EXPECT r13 = 0x80000000   and R = -83, r3
// EXPECT r14 = 0xffffffffffffffad   or R = -83, r3
// EXPECT r15 = 0xffffffff7fffffad   xor R = -83, r3
// EXPECT r16 = 0xffffffff7fffffad   andcm R = -83, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x8000000080000000   or R = r2, r3
// EXPECT r19 = 0x8000000080000000   xor R = r2, r3
// EXPECT r20 = 0x8000000000000000   andcm R = r2, r3
// EXPECT r21 = 0x7fffeffb   adds R = -4101, r3
// EXPECT r22 = 0xffffffffffe28099   addl R = -1933159, r1
// EXPECT r23 = 0x80000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x4000000080000000   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x80000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x4000000080000000   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x80000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x4000000080000000   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x80000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x4000000080000000   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x4000000080000000   addp4 R = r2, r3
// EXPECT r32 = 0x400000007fffeffb   addp4 R = -4101, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x0   extr.u R = r3, 58, 21
// EXPECT r37 = 0x0   extr R = r3, 58, 21
// EXPECT r38 = 0x0   dep.z R = r2, 58, 21
// EXPECT r39 = 0xbc00000000000000   dep.z R = 47, 58, 21
// EXPECT r40 = 0x80000000   dep R = r2, r3, 58, 13
// EXPECT r41 = 0xfc00000080000000   dep R = -1, r3, 58, 21
// EXPECT r42 = 0x1000   shrp R = r2, r3, 19
// EXPECT r43 = 0x0   shl R = r2, 19
// EXPECT r44 = 0x1000   shr.u R = r3, 19
// EXPECT r45 = 0x1000   shr R = r3, 19
// EXPECT r46 = 0x0   extr.u R = r3, 23, 5
// EXPECT r47 = 0x0   extr R = r3, 23, 5
// EXPECT r48 = 0x0   dep.z R = r2, 23, 5
// EXPECT r49 = 0x8000000   dep.z R = 112, 23, 5
// EXPECT r50 = 0x80000000   dep R = r2, r3, 23, 8
// EXPECT r51 = 0x80000000   dep R = 0, r3, 23, 5
// EXPECT r52 = 0x40   shrp R = r2, r3, 25
// EXPECT r53 = 0x0   shl R = r2, 25
// EXPECT r54 = 0x40   shr.u R = r3, 25
// EXPECT r55 = 0x40   shr R = r3, 25
// EXPECT r56 = 0x20   extr.u R = r3, 26, 10
// EXPECT r57 = 0x20   extr R = r3, 26, 10
// EXPECT r58 = 0x0   dep.z R = r2, 26, 10
// EXPECT r59 = 0xef8000000   dep.z R = -66, 26, 10
// EXPECT r60 = 0x0   dep R = r2, r3, 26, 16
// EXPECT r61 = 0xffc000000   dep R = -1, r3, 26, 10
// EXPECT r62 = 0x40   shrp R = r2, r3, 25
// EXPECT r63 = 0x0   shl R = r2, 25
// EXPECT r64 = 0x40   shr.u R = r3, 25
// EXPECT r65 = 0x40   shr R = r3, 25
// EXPECT r66 = 0x0   extr.u R = r3, 43, 19
// EXPECT r67 = 0x0   extr R = r3, 43, 19
// EXPECT r68 = 0x0   dep.z R = r2, 43, 19
// EXPECT r69 = 0x2880000000000   dep.z R = 81, 43, 19
// EXPECT r70 = 0x80000000   dep R = r2, r3, 43, 10
// EXPECT r71 = 0x3ffff80080000000   dep R = -1, r3, 43, 19
// EXPECT r72 = 0x0   shrp R = r2, r3, 57
// EXPECT r73 = 0x0   shl R = r2, 57
// EXPECT r74 = 0x0   shr.u R = r3, 57
// EXPECT r75 = 0x0   shr R = r3, 57
// EXPECT r76 = 0x0   extr.u R = r3, 49, 41
// EXPECT r77 = 0x0   extr R = r3, 49, 41
// EXPECT r78 = 0x0   dep.z R = r2, 49, 41
// EXPECT r79 = 0xf0000000000000   dep.z R = 120, 49, 41
// EXPECT r80 = 0x80000000   dep R = r2, r3, 49, 13
// EXPECT r81 = 0xfffe000080000000   dep R = -1, r3, 49, 41
// EXPECT r82 = 0x10000000   shrp R = r2, r3, 3
// EXPECT r83 = 0x0   shl R = r2, 3
// EXPECT r84 = 0x10000000   shr.u R = r3, 3
// EXPECT r85 = 0x10000000   shr R = r3, 3
// EXPECT r86 = 0x100000   extr.u R = r3, 11, 21
// EXPECT r87 = 0xfffffffffff00000   extr R = r3, 11, 21
// EXPECT r88 = 0x0   dep.z R = r2, 11, 21
// EXPECT r89 = 0x3e800   dep.z R = 125, 11, 21
// EXPECT r90 = 0x80000000   dep R = r2, r3, 11, 1
// EXPECT r91 = 0xfffff800   dep R = -1, r3, 11, 21
// EXPECT r92 = 0x800000   shrp R = r2, r3, 8
// EXPECT r93 = 0x0   shl R = r2, 8
// EXPECT r94 = 0x800000   shr.u R = r3, 8
// EXPECT r95 = 0x800000   shr R = r3, 8
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0xffffffff80000000   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x80000000   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p9 = 1
// EXPECT p10 = 0   tbit.nz P1, P2 = r3, 1
// EXPECT p11 = 1
// EXPECT p12 = 0   cmp.gtu P1, P2 = 49, r3
// EXPECT p13 = 1
// EXPECT p14 = 0   tbit.nz P1, P2 = r3, 44
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp.leu P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 1   cmp4.gtu P1, P2 = -110, r3
// EXPECT p19 = 0
// EXPECT p20 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p27 = 0
// EXPECT p28 = 1   cmp.ne P1, P2 = 35, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp.geu P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   tbit.z P1, P2 = r3, 20
// EXPECT p37 = 0
// EXPECT p38 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p39 = 0
// EXPECT p40 = 0   cmp.gt P1, P2 = 42, r3
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.ltu P1, P2 = -36, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp.lt P1, P2 = -71, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.gt P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 1   cmp4.leu P1, P2 = 63, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   tbit.z P1, P2 = r3, 44
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   tbit.nz P1, P2 = r3, 20
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp4.ne P1, P2 = -42, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   tbit.z P1, P2 = r3, 1
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp.eq P1, P2 = -15, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x8000000000000000
  movl r3 = 0x80000000
  movl r4 = 0x5f204ab5e5284e4
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -83, r3
  and r13 = -83, r3
  or r14 = -83, r3
  xor r15 = -83, r3
  andcm r16 = -83, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -4101, r3
  addl r22 = -1933159, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -4101, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 58, 21
  extr r37 = r3, 58, 21
  dep.z r38 = r2, 58, 21
  dep.z r39 = 47, 58, 21
  dep r40 = r2, r3, 58, 13
  dep r41 = -1, r3, 58, 21
  shrp r42 = r2, r3, 19
  shl r43 = r2, 19
  shr.u r44 = r3, 19
  shr r45 = r3, 19
  extr.u r46 = r3, 23, 5
  extr r47 = r3, 23, 5
  dep.z r48 = r2, 23, 5
  dep.z r49 = 112, 23, 5
  dep r50 = r2, r3, 23, 8
  dep r51 = 0, r3, 23, 5
  shrp r52 = r2, r3, 25
  shl r53 = r2, 25
  shr.u r54 = r3, 25
  shr r55 = r3, 25
  extr.u r56 = r3, 26, 10
  extr r57 = r3, 26, 10
  dep.z r58 = r2, 26, 10
  dep.z r59 = -66, 26, 10
  dep r60 = r2, r3, 26, 16
  dep r61 = -1, r3, 26, 10
  shrp r62 = r2, r3, 25
  shl r63 = r2, 25
  shr.u r64 = r3, 25
  shr r65 = r3, 25
  extr.u r66 = r3, 43, 19
  extr r67 = r3, 43, 19
  dep.z r68 = r2, 43, 19
  dep.z r69 = 81, 43, 19
  dep r70 = r2, r3, 43, 10
  dep r71 = -1, r3, 43, 19
  shrp r72 = r2, r3, 57
  shl r73 = r2, 57
  shr.u r74 = r3, 57
  shr r75 = r3, 57
  extr.u r76 = r3, 49, 41
  extr r77 = r3, 49, 41
  dep.z r78 = r2, 49, 41
  dep.z r79 = 120, 49, 41
  dep r80 = r2, r3, 49, 13
  dep r81 = -1, r3, 49, 41
  shrp r82 = r2, r3, 3
  shl r83 = r2, 3
  shr.u r84 = r3, 3
  shr r85 = r3, 3
  extr.u r86 = r3, 11, 21
  extr r87 = r3, 11, 21
  dep.z r88 = r2, 11, 21
  dep.z r89 = 125, 11, 21
  dep r90 = r2, r3, 11, 1
  dep r91 = -1, r3, 11, 21
  shrp r92 = r2, r3, 8
  shl r93 = r2, 8
  shr.u r94 = r3, 8
  shr r95 = r3, 8
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = r2, r3
  cmp4.lt p8, p9 = r2, r3
  tbit.nz p10, p11 = r3, 1
  cmp.gtu p12, p13 = 49, r3
  tbit.nz p14, p15 = r3, 44
  cmp.leu p16, p17 = r2, r3
  cmp4.gtu p18, p19 = -110, r3
  cmp.lt p20, p21 = r2, r3
  cmp4.le p22, p23 = r2, r3
  cmp4.ltu p24, p25 = r2, r3
  cmp4.leu p26, p27 = r2, r3
  cmp.ne p28, p29 = 35, r3
  cmp.geu p30, p31 = r2, r3
  cmp4.ge p32, p33 = r2, r3
  cmp.ge p34, p35 = r2, r3
  tbit.z p36, p37 = r3, 20
  cmp.ne p38, p39 = r2, r3
  cmp.gt p40, p41 = 42, r3
  cmp.ltu p42, p43 = -36, r3
  cmp.lt p44, p45 = -71, r3
  cmp.gt p46, p47 = r2, r3
  cmp4.leu p48, p49 = 63, r3
  tbit.z p50, p51 = r3, 44
  cmp4.eq p52, p53 = r2, r3
  tbit.nz p54, p55 = r3, 20
  cmp4.ne p56, p57 = -42, r3
  tbit.z p58, p59 = r3, 1
  cmp.eq p60, p61 = -15, r3
  cmp.le p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
