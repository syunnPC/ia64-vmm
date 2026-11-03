/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xffffffffffffffff   add R = r2, r3
// EXPECT r9 = 0x0   add R = r2, r3, 1
// EXPECT r10 = 0x3   sub R = r2, r3
// EXPECT r11 = 0x2   sub R = r2, r3, 1
// EXPECT r12 = 0x47   sub R = 69, r3
// EXPECT r13 = 0x44   and R = 69, r3
// EXPECT r14 = 0xffffffffffffffff   or R = 69, r3
// EXPECT r15 = 0xffffffffffffffbb   xor R = 69, r3
// EXPECT r16 = 0x1   andcm R = 69, r3
// EXPECT r17 = 0x0   and R = r2, r3
// EXPECT r18 = 0xffffffffffffffff   or R = r2, r3
// EXPECT r19 = 0xffffffffffffffff   xor R = r2, r3
// EXPECT r20 = 0x1   andcm R = r2, r3
// EXPECT r21 = 0x1f4c   adds R = 8014, r3
// EXPECT r22 = 0xfffffffffff1be52   addl R = -934318, r1
// EXPECT r23 = 0x0   shladd R = r2, 1, r3
// EXPECT r24 = 0x6000000000000000   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x2   shladd R = r2, 2, r3
// EXPECT r26 = 0x6000000000000002   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x6   shladd R = r2, 3, r3
// EXPECT r28 = 0x6000000000000006   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xe   shladd R = r2, 4, r3
// EXPECT r30 = 0x600000000000000e   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x60000000ffffffff   addp4 R = r2, r3
// EXPECT r32 = 0x6000000000001f4c   addp4 R = 8014, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0x1fffffffff   extr.u R = r3, 27, 54
// EXPECT r37 = 0xffffffffffffffff   extr R = r3, 27, 54
// EXPECT r38 = 0x8000000   dep.z R = r2, 27, 54
// EXPECT r39 = 0x1f0000000   dep.z R = 62, 27, 54
// EXPECT r40 = 0xfffffffc0ffffffe   dep R = r2, r3, 27, 7
// EXPECT r41 = 0xfffffffffffffffe   dep R = -1, r3, 27, 54
// EXPECT r42 = 0x1ffffffff   shrp R = r2, r3, 32
// EXPECT r43 = 0x100000000   shl R = r2, 32
// EXPECT r44 = 0xffffffff   shr.u R = r3, 32
// EXPECT r45 = 0xffffffffffffffff   shr R = r3, 32
// EXPECT r46 = 0x7fffffff   extr.u R = r3, 4, 31
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 4, 31
// EXPECT r48 = 0x10   dep.z R = r2, 4, 31
// EXPECT r49 = 0x6c0   dep.z R = 108, 4, 31
// EXPECT r50 = 0xffffffffffffc01e   dep R = r2, r3, 4, 10
// EXPECT r51 = 0xfffffffffffffffe   dep R = -1, r3, 4, 31
// EXPECT r52 = 0x7fffffffffffff   shrp R = r2, r3, 10
// EXPECT r53 = 0x400   shl R = r2, 10
// EXPECT r54 = 0x3fffffffffffff   shr.u R = r3, 10
// EXPECT r55 = 0xffffffffffffffff   shr R = r3, 10
// EXPECT r56 = 0x3ffffffff   extr.u R = r3, 30, 39
// EXPECT r57 = 0xffffffffffffffff   extr R = r3, 30, 39
// EXPECT r58 = 0x40000000   dep.z R = r2, 30, 39
// EXPECT r59 = 0xfffffffa00000000   dep.z R = -24, 30, 39
// EXPECT r60 = 0xfffffff07ffffffe   dep R = r2, r3, 30, 6
// EXPECT r61 = 0x3ffffffe   dep R = 0, r3, 30, 39
// EXPECT r62 = 0x1ffffffffffffff   shrp R = r2, r3, 8
// EXPECT r63 = 0x100   shl R = r2, 8
// EXPECT r64 = 0xffffffffffffff   shr.u R = r3, 8
// EXPECT r65 = 0xffffffffffffffff   shr R = r3, 8
// EXPECT r66 = 0x3ffffffff   extr.u R = r3, 30, 48
// EXPECT r67 = 0xffffffffffffffff   extr R = r3, 30, 48
// EXPECT r68 = 0x40000000   dep.z R = r2, 30, 48
// EXPECT r69 = 0x1780000000   dep.z R = 94, 30, 48
// EXPECT r70 = 0xfffffffffffffffe   dep R = r2, r3, 30, 1
// EXPECT r71 = 0xfffffffffffffffe   dep R = -1, r3, 30, 48
// EXPECT r72 = 0x1fffff   shrp R = r2, r3, 44
// EXPECT r73 = 0x100000000000   shl R = r2, 44
// EXPECT r74 = 0xfffff   shr.u R = r3, 44
// EXPECT r75 = 0xffffffffffffffff   shr R = r3, 44
// EXPECT r76 = 0x3   extr.u R = r3, 62, 22
// EXPECT r77 = 0xffffffffffffffff   extr R = r3, 62, 22
// EXPECT r78 = 0x4000000000000000   dep.z R = r2, 62, 22
// EXPECT r79 = 0x4000000000000000   dep.z R = 1, 62, 22
// EXPECT r80 = 0x7ffffffffffffffe   dep R = r2, r3, 62, 3
// EXPECT r81 = 0x3ffffffffffffffe   dep R = 0, r3, 62, 22
// EXPECT r82 = 0x7f   shrp R = r2, r3, 58
// EXPECT r83 = 0x400000000000000   shl R = r2, 58
// EXPECT r84 = 0x3f   shr.u R = r3, 58
// EXPECT r85 = 0xffffffffffffffff   shr R = r3, 58
// EXPECT r86 = 0x1fffffff   extr.u R = r3, 26, 29
// EXPECT r87 = 0xffffffffffffffff   extr R = r3, 26, 29
// EXPECT r88 = 0x4000000   dep.z R = r2, 26, 29
// EXPECT r89 = 0xd4000000   dep.z R = 53, 26, 29
// EXPECT r90 = 0xffffffff87fffffe   dep R = r2, r3, 26, 5
// EXPECT r91 = 0xff80000003fffffe   dep R = 0, r3, 26, 29
// EXPECT r92 = 0x3ffffffffff   shrp R = r2, r3, 23
// EXPECT r93 = 0x800000   shl R = r2, 23
// EXPECT r94 = 0x1ffffffffff   shr.u R = r3, 23
// EXPECT r95 = 0xffffffffffffffff   shr R = r3, 23
// EXPECT r96 = 0xfffffffffffffffe   sxt1 R = r3
// EXPECT r97 = 0xfffffffffffffffe   sxt2 R = r3
// EXPECT r98 = 0xfffffffffffffffe   sxt4 R = r3
// EXPECT r99 = 0xfe   zxt1 R = r3
// EXPECT r100 = 0xfffe   zxt2 R = r3
// EXPECT r101 = 0xfffffffe   zxt4 R = r3
// EXPECT r102 = 0x3f   popcnt R = r3
// EXPECT p6 = 1   cmp4.ltu P1, P2 = -68, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp.ltu P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 1   tbit.nz P1, P2 = r3, 25
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp.leu P1, P2 = -127, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp.ltu P1, P2 = 15, r3
// EXPECT p15 = 0
// EXPECT p16 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp4.le P1, P2 = 25, r3
// EXPECT p19 = 1
// EXPECT p20 = 0   cmp.geu P1, P2 = -111, r3
// EXPECT p21 = 1
// EXPECT p22 = 0   tbit.z P1, P2 = r3, 36
// EXPECT p23 = 1
// EXPECT p24 = 0   cmp4.gtu P1, P2 = -33, r3
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp4.le P1, P2 = r2, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p31 = 0
// EXPECT p32 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p33 = 1
// EXPECT p34 = 0   cmp.lt P1, P2 = 13, r3
// EXPECT p35 = 1
// EXPECT p36 = 1   cmp.ne P1, P2 = 114, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   tbit.z P1, P2 = r3, 17
// EXPECT p41 = 1
// EXPECT p42 = 0   cmp.gtu P1, P2 = -98, r3
// EXPECT p43 = 1
// EXPECT p44 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p45 = 0
// EXPECT p46 = 1   tbit.nz P1, P2 = r3, 36
// EXPECT p47 = 0
// EXPECT p48 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p49 = 0
// EXPECT p50 = 1   cmp.gt P1, P2 = 25, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   tbit.z P1, P2 = r3, 57
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p57 = 1
// EXPECT p58 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p59 = 0
// EXPECT p60 = 0   cmp.le P1, P2 = 23, r3
// EXPECT p61 = 1
// EXPECT p62 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p63 = 0
.text
.global _start
_start:
  movl r2 = 0x1
  movl r3 = 0xfffffffffffffffe
  movl r4 = 0xffffffffffffffff
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = 69, r3
  and r13 = 69, r3
  or r14 = 69, r3
  xor r15 = 69, r3
  andcm r16 = 69, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 8014, r3
  addl r22 = -934318, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 8014, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 27, 54
  extr r37 = r3, 27, 54
  dep.z r38 = r2, 27, 54
  dep.z r39 = 62, 27, 54
  dep r40 = r2, r3, 27, 7
  dep r41 = -1, r3, 27, 54
  shrp r42 = r2, r3, 32
  shl r43 = r2, 32
  shr.u r44 = r3, 32
  shr r45 = r3, 32
  extr.u r46 = r3, 4, 31
  extr r47 = r3, 4, 31
  dep.z r48 = r2, 4, 31
  dep.z r49 = 108, 4, 31
  dep r50 = r2, r3, 4, 10
  dep r51 = -1, r3, 4, 31
  shrp r52 = r2, r3, 10
  shl r53 = r2, 10
  shr.u r54 = r3, 10
  shr r55 = r3, 10
  extr.u r56 = r3, 30, 39
  extr r57 = r3, 30, 39
  dep.z r58 = r2, 30, 39
  dep.z r59 = -24, 30, 39
  dep r60 = r2, r3, 30, 6
  dep r61 = 0, r3, 30, 39
  shrp r62 = r2, r3, 8
  shl r63 = r2, 8
  shr.u r64 = r3, 8
  shr r65 = r3, 8
  extr.u r66 = r3, 30, 48
  extr r67 = r3, 30, 48
  dep.z r68 = r2, 30, 48
  dep.z r69 = 94, 30, 48
  dep r70 = r2, r3, 30, 1
  dep r71 = -1, r3, 30, 48
  shrp r72 = r2, r3, 44
  shl r73 = r2, 44
  shr.u r74 = r3, 44
  shr r75 = r3, 44
  extr.u r76 = r3, 62, 22
  extr r77 = r3, 62, 22
  dep.z r78 = r2, 62, 22
  dep.z r79 = 1, 62, 22
  dep r80 = r2, r3, 62, 3
  dep r81 = 0, r3, 62, 22
  shrp r82 = r2, r3, 58
  shl r83 = r2, 58
  shr.u r84 = r3, 58
  shr r85 = r3, 58
  extr.u r86 = r3, 26, 29
  extr r87 = r3, 26, 29
  dep.z r88 = r2, 26, 29
  dep.z r89 = 53, 26, 29
  dep r90 = r2, r3, 26, 5
  dep r91 = 0, r3, 26, 29
  shrp r92 = r2, r3, 23
  shl r93 = r2, 23
  shr.u r94 = r3, 23
  shr r95 = r3, 23
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.ltu p6, p7 = -68, r3
  cmp.ltu p8, p9 = r2, r3
  tbit.nz p10, p11 = r3, 25
  cmp.leu p12, p13 = -127, r3
  cmp.ltu p14, p15 = 15, r3
  cmp.le p16, p17 = r2, r3
  cmp4.le p18, p19 = 25, r3
  cmp.geu p20, p21 = -111, r3
  tbit.z p22, p23 = r3, 36
  cmp4.gtu p24, p25 = -33, r3
  cmp4.le p26, p27 = r2, r3
  cmp.ne p28, p29 = r2, r3
  cmp.leu p30, p31 = r2, r3
  cmp4.lt p32, p33 = r2, r3
  cmp.lt p34, p35 = 13, r3
  cmp.ne p36, p37 = 114, r3
  cmp4.eq p38, p39 = r2, r3
  tbit.z p40, p41 = r3, 17
  cmp.gtu p42, p43 = -98, r3
  cmp4.leu p44, p45 = r2, r3
  tbit.nz p46, p47 = r3, 36
  cmp4.gt p48, p49 = r2, r3
  cmp.gt p50, p51 = 25, r3
  tbit.z p52, p53 = r3, 57
  cmp.lt p54, p55 = r2, r3
  cmp4.gtu p56, p57 = r2, r3
  cmp4.ne p58, p59 = r2, r3
  cmp.le p60, p61 = 23, r3
  cmp4.ltu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
