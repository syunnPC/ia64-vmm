/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x100000000   add R = r2, r3
// EXPECT r9 = 0x100000001   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff00000000   sub R = r2, r3
// EXPECT r11 = 0xfffffffeffffffff   sub R = r2, r3, 1
// EXPECT r12 = 0xfffffffeffffffb3   sub R = -77, r3
// EXPECT r13 = 0x100000000   and R = -77, r3
// EXPECT r14 = 0xffffffffffffffb3   or R = -77, r3
// EXPECT r15 = 0xfffffffeffffffb3   xor R = -77, r3
// EXPECT r16 = 0xfffffffeffffffb3   andcm R = -77, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0x100000000   or R = r2, r3
// EXPECT r19 = 0x100000000   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0xfffff7cb   adds R = -2101, r3
// EXPECT r22 = 0x5f01e   addl R = 389150, r1
// EXPECT r23 = 0x100000000   shladd R = r2, 1, r3
// EXPECT r24 = 0x0   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x100000000   shladd R = r2, 2, r3
// EXPECT r26 = 0x0   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x100000000   shladd R = r2, 3, r3
// EXPECT r28 = 0x0   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x100000000   shladd R = r2, 4, r3
// EXPECT r30 = 0x0   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x0   addp4 R = r2, r3
// EXPECT r32 = 0xfffff7cb   addp4 R = -2101, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x20000   extr.u R = r3, 15, 43
// EXPECT r37 = 0x20000   extr R = r3, 15, 43
// EXPECT r38 = 0x0   dep.z R = r2, 15, 43
// EXPECT r39 = 0x2c0000   dep.z R = 88, 15, 43
// EXPECT r40 = 0x100000000   dep R = r2, r3, 15, 7
// EXPECT r41 = 0x3ffffffffff8000   dep R = -1, r3, 15, 43
// EXPECT r42 = 0x0   shrp R = r2, r3, 36
// EXPECT r43 = 0x0   shl R = r2, 36
// EXPECT r44 = 0x0   shr.u R = r3, 36
// EXPECT r45 = 0x0   shr R = r3, 36
// EXPECT r46 = 0x0   extr.u R = r3, 63, 51
// EXPECT r47 = 0x0   extr R = r3, 63, 51
// EXPECT r48 = 0x0   dep.z R = r2, 63, 51
// EXPECT r49 = 0x8000000000000000   dep.z R = -111, 63, 51
// EXPECT r50 = 0x100000000   dep R = r2, r3, 63, 16
// EXPECT r51 = 0x100000000   dep R = 0, r3, 63, 51
// EXPECT r52 = 0x0   shrp R = r2, r3, 51
// EXPECT r53 = 0x0   shl R = r2, 51
// EXPECT r54 = 0x0   shr.u R = r3, 51
// EXPECT r55 = 0x0   shr R = r3, 51
// EXPECT r56 = 0x0   extr.u R = r3, 53, 23
// EXPECT r57 = 0x0   extr R = r3, 53, 23
// EXPECT r58 = 0x0   dep.z R = r2, 53, 23
// EXPECT r59 = 0x760000000000000   dep.z R = 59, 53, 23
// EXPECT r60 = 0x100000000   dep R = r2, r3, 53, 12
// EXPECT r61 = 0x100000000   dep R = 0, r3, 53, 23
// EXPECT r62 = 0x0   shrp R = r2, r3, 56
// EXPECT r63 = 0x0   shl R = r2, 56
// EXPECT r64 = 0x0   shr.u R = r3, 56
// EXPECT r65 = 0x0   shr R = r3, 56
// EXPECT r66 = 0x80000   extr.u R = r3, 13, 21
// EXPECT r67 = 0x80000   extr R = r3, 13, 21
// EXPECT r68 = 0x0   dep.z R = r2, 13, 21
// EXPECT r69 = 0x92000   dep.z R = 73, 13, 21
// EXPECT r70 = 0x100000000   dep R = r2, r3, 13, 12
// EXPECT r71 = 0x3ffffe000   dep R = -1, r3, 13, 21
// EXPECT r72 = 0x20000000   shrp R = r2, r3, 3
// EXPECT r73 = 0x0   shl R = r2, 3
// EXPECT r74 = 0x20000000   shr.u R = r3, 3
// EXPECT r75 = 0x20000000   shr R = r3, 3
// EXPECT r76 = 0x0   extr.u R = r3, 60, 6
// EXPECT r77 = 0x0   extr R = r3, 60, 6
// EXPECT r78 = 0x0   dep.z R = r2, 60, 6
// EXPECT r79 = 0xd000000000000000   dep.z R = 29, 60, 6
// EXPECT r80 = 0x100000000   dep R = r2, r3, 60, 13
// EXPECT r81 = 0x100000000   dep R = 0, r3, 60, 6
// EXPECT r82 = 0x800   shrp R = r2, r3, 21
// EXPECT r83 = 0x0   shl R = r2, 21
// EXPECT r84 = 0x800   shr.u R = r3, 21
// EXPECT r85 = 0x800   shr R = r3, 21
// EXPECT r86 = 0x0   extr.u R = r3, 29, 2
// EXPECT r87 = 0x0   extr R = r3, 29, 2
// EXPECT r88 = 0x0   dep.z R = r2, 29, 2
// EXPECT r89 = 0x40000000   dep.z R = -26, 29, 2
// EXPECT r90 = 0x0   dep R = r2, r3, 29, 8
// EXPECT r91 = 0x160000000   dep R = -1, r3, 29, 2
// EXPECT r92 = 0x0   shrp R = r2, r3, 44
// EXPECT r93 = 0x0   shl R = r2, 44
// EXPECT r94 = 0x0   shr.u R = r3, 44
// EXPECT r95 = 0x0   shr R = r3, 44
// EXPECT r96 = 0x0   sxt1 R = r3
// EXPECT r97 = 0x0   sxt2 R = r3
// EXPECT r98 = 0x0   sxt4 R = r3
// EXPECT r99 = 0x0   zxt1 R = r3
// EXPECT r100 = 0x0   zxt2 R = r3
// EXPECT r101 = 0x0   zxt4 R = r3
// EXPECT r102 = 0x1   popcnt R = r3
// EXPECT p6 = 1   cmp.lt P1, P2 = 9, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp4.gtu P1, P2 = -128, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.ge P1, P2 = r2, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   tbit.z P1, P2 = r3, 29
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 1   tbit.z P1, P2 = r3, 23
// EXPECT p19 = 0
// EXPECT p20 = 0   cmp.eq P1, P2 = 52, r3
// EXPECT p21 = 1
// EXPECT p22 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p23 = 0
// EXPECT p24 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.ge P1, P2 = -100, r3
// EXPECT p27 = 1
// EXPECT p28 = 0   cmp.gtu P1, P2 = 90, r3
// EXPECT p29 = 1
// EXPECT p30 = 1   cmp4.ge P1, P2 = 41, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp.ltu P1, P2 = -126, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.leu P1, P2 = -62, r3
// EXPECT p35 = 1
// EXPECT p36 = 0   cmp4.ne P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   tbit.nz P1, P2 = r3, 22
// EXPECT p41 = 1
// EXPECT p42 = 1   tbit.z P1, P2 = r3, 3
// EXPECT p43 = 0
// EXPECT p44 = 0   cmp4.le P1, P2 = 54, r3
// EXPECT p45 = 1
// EXPECT p46 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 1   tbit.z P1, P2 = r3, 22
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp4.geu P1, P2 = 106, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   cmp4.eq P1, P2 = 58, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp4.ltu P1, P2 = 120, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp4.eq P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp.le P1, P2 = 68, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x0
  movl r3 = 0x100000000
  movl r4 = 0x41
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -77, r3
  and r13 = -77, r3
  or r14 = -77, r3
  xor r15 = -77, r3
  andcm r16 = -77, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -2101, r3
  addl r22 = 389150, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -2101, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 15, 43
  extr r37 = r3, 15, 43
  dep.z r38 = r2, 15, 43
  dep.z r39 = 88, 15, 43
  dep r40 = r2, r3, 15, 7
  dep r41 = -1, r3, 15, 43
  shrp r42 = r2, r3, 36
  shl r43 = r2, 36
  shr.u r44 = r3, 36
  shr r45 = r3, 36
  extr.u r46 = r3, 63, 51
  extr r47 = r3, 63, 51
  dep.z r48 = r2, 63, 51
  dep.z r49 = -111, 63, 51
  dep r50 = r2, r3, 63, 16
  dep r51 = 0, r3, 63, 51
  shrp r52 = r2, r3, 51
  shl r53 = r2, 51
  shr.u r54 = r3, 51
  shr r55 = r3, 51
  extr.u r56 = r3, 53, 23
  extr r57 = r3, 53, 23
  dep.z r58 = r2, 53, 23
  dep.z r59 = 59, 53, 23
  dep r60 = r2, r3, 53, 12
  dep r61 = 0, r3, 53, 23
  shrp r62 = r2, r3, 56
  shl r63 = r2, 56
  shr.u r64 = r3, 56
  shr r65 = r3, 56
  extr.u r66 = r3, 13, 21
  extr r67 = r3, 13, 21
  dep.z r68 = r2, 13, 21
  dep.z r69 = 73, 13, 21
  dep r70 = r2, r3, 13, 12
  dep r71 = -1, r3, 13, 21
  shrp r72 = r2, r3, 3
  shl r73 = r2, 3
  shr.u r74 = r3, 3
  shr r75 = r3, 3
  extr.u r76 = r3, 60, 6
  extr r77 = r3, 60, 6
  dep.z r78 = r2, 60, 6
  dep.z r79 = 29, 60, 6
  dep r80 = r2, r3, 60, 13
  dep r81 = 0, r3, 60, 6
  shrp r82 = r2, r3, 21
  shl r83 = r2, 21
  shr.u r84 = r3, 21
  shr r85 = r3, 21
  extr.u r86 = r3, 29, 2
  extr r87 = r3, 29, 2
  dep.z r88 = r2, 29, 2
  dep.z r89 = -26, 29, 2
  dep r90 = r2, r3, 29, 8
  dep r91 = -1, r3, 29, 2
  shrp r92 = r2, r3, 44
  shl r93 = r2, 44
  shr.u r94 = r3, 44
  shr r95 = r3, 44
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp.lt p6, p7 = 9, r3
  cmp4.gtu p8, p9 = -128, r3
  cmp.ge p10, p11 = r2, r3
  cmp.ne p12, p13 = r2, r3
  tbit.z p14, p15 = r3, 29
  cmp.lt p16, p17 = r2, r3
  tbit.z p18, p19 = r3, 23
  cmp.eq p20, p21 = 52, r3
  cmp4.le p22, p23 = r2, r3
  cmp.eq p24, p25 = r2, r3
  cmp.ge p26, p27 = -100, r3
  cmp.gtu p28, p29 = 90, r3
  cmp4.ge p30, p31 = 41, r3
  cmp.ltu p32, p33 = -126, r3
  cmp.leu p34, p35 = -62, r3
  cmp4.ne p36, p37 = r2, r3
  cmp.geu p38, p39 = r2, r3
  tbit.nz p40, p41 = r3, 22
  tbit.z p42, p43 = r3, 3
  cmp4.le p44, p45 = 54, r3
  cmp.le p46, p47 = r2, r3
  tbit.z p48, p49 = r3, 22
  cmp4.geu p50, p51 = 106, r3
  cmp4.eq p52, p53 = 58, r3
  cmp4.ltu p54, p55 = 120, r3
  cmp4.eq p56, p57 = r2, r3
  cmp.le p58, p59 = 68, r3
  cmp.gtu p60, p61 = r2, r3
  cmp.leu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
