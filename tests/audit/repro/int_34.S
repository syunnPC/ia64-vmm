/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0xd6be75b17ffffffe   add R = r2, r3
// EXPECT r9 = 0xd6be75b17fffffff   add R = r2, r3, 1
// EXPECT r10 = 0x29418a4f80000000   sub R = r2, r3
// EXPECT r11 = 0x29418a4f7fffffff   sub R = r2, r3, 1
// EXPECT r12 = 0x29418a4effffffed   sub R = -20, r3
// EXPECT r13 = 0xd6be75b0ffffffec   and R = -20, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -20, r3
// EXPECT r15 = 0x29418a4f00000013   xor R = -20, r3
// EXPECT r16 = 0x29418a4f00000000   andcm R = -20, r3
// EXPECT r17 = 0x7fffffff   and R = r2, r3
// EXPECT r18 = 0xd6be75b0ffffffff   or R = r2, r3
// EXPECT r19 = 0xd6be75b080000000   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0xd6be75b0fffffa5d   adds R = -1442, r3
// EXPECT r22 = 0x19ffb7   addl R = 1703863, r1
// EXPECT r23 = 0xd6be75b1fffffffd   shladd R = r2, 1, r3
// EXPECT r24 = 0x60000000fffffffd   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0xd6be75b2fffffffb   shladd R = r2, 2, r3
// EXPECT r26 = 0x60000000fffffffb   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0xd6be75b4fffffff7   shladd R = r2, 3, r3
// EXPECT r28 = 0x60000000fffffff7   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0xd6be75b8ffffffef   shladd R = r2, 4, r3
// EXPECT r30 = 0x60000000ffffffef   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x600000007ffffffe   addp4 R = r2, r3
// EXPECT r32 = 0x60000000fffffa5d   addp4 R = -1442, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0xffffffffffffffff   shr R = r3, r4
// EXPECT r36 = 0xc3ff   extr.u R = r3, 22, 17
// EXPECT r37 = 0xc3ff   extr R = r3, 22, 17
// EXPECT r38 = 0x7fffc00000   dep.z R = r2, 22, 17
// EXPECT r39 = 0x12000000   dep.z R = 72, 22, 17
// EXPECT r40 = 0xd6be75bfffffffff   dep R = r2, r3, 22, 14
// EXPECT r41 = 0xd6be7580003fffff   dep R = 0, r3, 22, 17
// EXPECT r42 = 0xffffffffad7ceb61   shrp R = r2, r3, 31
// EXPECT r43 = 0x3fffffff80000000   shl R = r2, 31
// EXPECT r44 = 0x1ad7ceb61   shr.u R = r3, 31
// EXPECT r45 = 0xffffffffad7ceb61   shr R = r3, 31
// EXPECT r46 = 0x3ff   extr.u R = r3, 14, 10
// EXPECT r47 = 0xffffffffffffffff   extr R = r3, 14, 10
// EXPECT r48 = 0xffc000   dep.z R = r2, 14, 10
// EXPECT r49 = 0x18c000   dep.z R = 99, 14, 10
// EXPECT r50 = 0xd6be75b0ffffffff   dep R = r2, r3, 14, 9
// EXPECT r51 = 0xd6be75b0ffffffff   dep R = -1, r3, 14, 10
// EXPECT r52 = 0xffff5af9d6c3ffff   shrp R = r2, r3, 14
// EXPECT r53 = 0x1fffffffc000   shl R = r2, 14
// EXPECT r54 = 0x35af9d6c3ffff   shr.u R = r3, 14
// EXPECT r55 = 0xffff5af9d6c3ffff   shr R = r3, 14
// EXPECT r56 = 0x16be75b   extr.u R = r3, 36, 26
// EXPECT r57 = 0x16be75b   extr R = r3, 36, 26
// EXPECT r58 = 0x3ffffff000000000   dep.z R = r2, 36, 26
// EXPECT r59 = 0x3ffffd9000000000   dep.z R = -39, 36, 26
// EXPECT r60 = 0xd6bffff0ffffffff   dep R = r2, r3, 36, 13
// EXPECT r61 = 0xfffffff0ffffffff   dep R = -1, r3, 36, 26
// EXPECT r62 = 0x3fffffffeb   shrp R = r2, r3, 57
// EXPECT r63 = 0xfe00000000000000   shl R = r2, 57
// EXPECT r64 = 0x6b   shr.u R = r3, 57
// EXPECT r65 = 0xffffffffffffffeb   shr R = r3, 57
// EXPECT r66 = 0x1ad7ceb61fffff   extr.u R = r3, 11, 61
// EXPECT r67 = 0xfffad7ceb61fffff   extr R = r3, 11, 61
// EXPECT r68 = 0x3fffffff800   dep.z R = r2, 11, 61
// EXPECT r69 = 0x2b800   dep.z R = 87, 11, 61
// EXPECT r70 = 0xd6be75b0ffffffff   dep R = r2, r3, 11, 15
// EXPECT r71 = 0xffffffffffffffff   dep R = -1, r3, 11, 61
// EXPECT r72 = 0xffffd6be75b0ffff   shrp R = r2, r3, 16
// EXPECT r73 = 0x7fffffff0000   shl R = r2, 16
// EXPECT r74 = 0xd6be75b0ffff   shr.u R = r3, 16
// EXPECT r75 = 0xffffd6be75b0ffff   shr R = r3, 16
// EXPECT r76 = 0xd6   extr.u R = r3, 56, 36
// EXPECT r77 = 0xffffffffffffffd6   extr R = r3, 56, 36
// EXPECT r78 = 0xff00000000000000   dep.z R = r2, 56, 36
// EXPECT r79 = 0xc700000000000000   dep.z R = -57, 56, 36
// EXPECT r80 = 0xffbe75b0ffffffff   dep R = r2, r3, 56, 8
// EXPECT r81 = 0xbe75b0ffffffff   dep R = 0, r3, 56, 36
// EXPECT r82 = 0x3fffffffeb5f3ad   shrp R = r2, r3, 37
// EXPECT r83 = 0xffffffe000000000   shl R = r2, 37
// EXPECT r84 = 0x6b5f3ad   shr.u R = r3, 37
// EXPECT r85 = 0xfffffffffeb5f3ad   shr R = r3, 37
// EXPECT r86 = 0xeb   extr.u R = r3, 39, 9
// EXPECT r87 = 0xeb   extr R = r3, 39, 9
// EXPECT r88 = 0xff8000000000   dep.z R = r2, 39, 9
// EXPECT r89 = 0xc8000000000   dep.z R = 25, 39, 9
// EXPECT r90 = 0xd6be77b0ffffffff   dep R = r2, r3, 39, 4
// EXPECT r91 = 0xd6beffb0ffffffff   dep R = -1, r3, 39, 9
// EXPECT r92 = 0x7fffffffd6b   shrp R = r2, r3, 52
// EXPECT r93 = 0xfff0000000000000   shl R = r2, 52
// EXPECT r94 = 0xd6b   shr.u R = r3, 52
// EXPECT r95 = 0xfffffffffffffd6b   shr R = r3, 52
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0xffffffffffffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0xffffffff   zxt4 R = r3
// EXPECT r102 = 0x33   popcnt R = r3
// EXPECT p6 = 1   cmp4.gt P1, P2 = r2, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   cmp.ge P1, P2 = r2, r3
// EXPECT p9 = 0
// EXPECT p10 = 0   cmp.lt P1, P2 = 30, r3
// EXPECT p11 = 1
// EXPECT p12 = 1   tbit.nz P1, P2 = r3, 20
// EXPECT p13 = 0
// EXPECT p14 = 0   cmp4.gtu P1, P2 = 14, r3
// EXPECT p15 = 1
// EXPECT p16 = 0   cmp4.le P1, P2 = 47, r3
// EXPECT p17 = 1
// EXPECT p18 = 0   cmp.le P1, P2 = 105, r3
// EXPECT p19 = 1
// EXPECT p20 = 1   tbit.nz P1, P2 = r3, 15
// EXPECT p21 = 0
// EXPECT p22 = 0   tbit.z P1, P2 = r3, 7
// EXPECT p23 = 1
// EXPECT p24 = 0   tbit.nz P1, P2 = r3, 34
// EXPECT p25 = 1
// EXPECT p26 = 0   cmp.leu P1, P2 = -95, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   cmp4.ne P1, P2 = -40, r3
// EXPECT p29 = 0
// EXPECT p30 = 0   cmp4.lt P1, P2 = r2, r3
// EXPECT p31 = 1
// EXPECT p32 = 0   cmp4.geu P1, P2 = 5, r3
// EXPECT p33 = 1
// EXPECT p34 = 1   cmp.ltu P1, P2 = 53, r3
// EXPECT p35 = 0
// EXPECT p36 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p37 = 1
// EXPECT p38 = 0   cmp4.eq P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 0   cmp.geu P1, P2 = 81, r3
// EXPECT p41 = 1
// EXPECT p42 = 1   tbit.z P1, P2 = r3, 34
// EXPECT p43 = 0
// EXPECT p44 = 1   cmp4.lt P1, P2 = -33, r3
// EXPECT p45 = 0
// EXPECT p46 = 0   cmp.lt P1, P2 = r2, r3
// EXPECT p47 = 1
// EXPECT p48 = 0   cmp.le P1, P2 = r2, r3
// EXPECT p49 = 1
// EXPECT p50 = 1   cmp4.ge P1, P2 = r2, r3
// EXPECT p51 = 0
// EXPECT p52 = 0   tbit.z P1, P2 = r3, 20
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p57 = 0
// EXPECT p58 = 1   cmp.ge P1, P2 = 44, r3
// EXPECT p59 = 0
// EXPECT p60 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p61 = 0
// EXPECT p62 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x7fffffff
  movl r3 = 0xd6be75b0ffffffff
  movl r4 = 0x40
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -20, r3
  and r13 = -20, r3
  or r14 = -20, r3
  xor r15 = -20, r3
  andcm r16 = -20, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = -1442, r3
  addl r22 = 1703863, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = -1442, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 22, 17
  extr r37 = r3, 22, 17
  dep.z r38 = r2, 22, 17
  dep.z r39 = 72, 22, 17
  dep r40 = r2, r3, 22, 14
  dep r41 = 0, r3, 22, 17
  shrp r42 = r2, r3, 31
  shl r43 = r2, 31
  shr.u r44 = r3, 31
  shr r45 = r3, 31
  extr.u r46 = r3, 14, 10
  extr r47 = r3, 14, 10
  dep.z r48 = r2, 14, 10
  dep.z r49 = 99, 14, 10
  dep r50 = r2, r3, 14, 9
  dep r51 = -1, r3, 14, 10
  shrp r52 = r2, r3, 14
  shl r53 = r2, 14
  shr.u r54 = r3, 14
  shr r55 = r3, 14
  extr.u r56 = r3, 36, 26
  extr r57 = r3, 36, 26
  dep.z r58 = r2, 36, 26
  dep.z r59 = -39, 36, 26
  dep r60 = r2, r3, 36, 13
  dep r61 = -1, r3, 36, 26
  shrp r62 = r2, r3, 57
  shl r63 = r2, 57
  shr.u r64 = r3, 57
  shr r65 = r3, 57
  extr.u r66 = r3, 11, 61
  extr r67 = r3, 11, 61
  dep.z r68 = r2, 11, 61
  dep.z r69 = 87, 11, 61
  dep r70 = r2, r3, 11, 15
  dep r71 = -1, r3, 11, 61
  shrp r72 = r2, r3, 16
  shl r73 = r2, 16
  shr.u r74 = r3, 16
  shr r75 = r3, 16
  extr.u r76 = r3, 56, 36
  extr r77 = r3, 56, 36
  dep.z r78 = r2, 56, 36
  dep.z r79 = -57, 56, 36
  dep r80 = r2, r3, 56, 8
  dep r81 = 0, r3, 56, 36
  shrp r82 = r2, r3, 37
  shl r83 = r2, 37
  shr.u r84 = r3, 37
  shr r85 = r3, 37
  extr.u r86 = r3, 39, 9
  extr r87 = r3, 39, 9
  dep.z r88 = r2, 39, 9
  dep.z r89 = 25, 39, 9
  dep r90 = r2, r3, 39, 4
  dep r91 = -1, r3, 39, 9
  shrp r92 = r2, r3, 52
  shl r93 = r2, 52
  shr.u r94 = r3, 52
  shr r95 = r3, 52
  sxt1 r96 = r3
  sxt2 r97 = r3
  sxt4 r98 = r3
  zxt1 r99 = r3
  zxt2 r100 = r3
  zxt4 r101 = r3
  popcnt r102 = r3
  cmp4.gt p6, p7 = r2, r3
  cmp.ge p8, p9 = r2, r3
  cmp.lt p10, p11 = 30, r3
  tbit.nz p12, p13 = r3, 20
  cmp4.gtu p14, p15 = 14, r3
  cmp4.le p16, p17 = 47, r3
  cmp.le p18, p19 = 105, r3
  tbit.nz p20, p21 = r3, 15
  tbit.z p22, p23 = r3, 7
  tbit.nz p24, p25 = r3, 34
  cmp.leu p26, p27 = -95, r3
  cmp4.ne p28, p29 = -40, r3
  cmp4.lt p30, p31 = r2, r3
  cmp4.geu p32, p33 = 5, r3
  cmp.ltu p34, p35 = 53, r3
  cmp4.geu p36, p37 = r2, r3
  cmp4.eq p38, p39 = r2, r3
  cmp.geu p40, p41 = 81, r3
  tbit.z p42, p43 = r3, 34
  cmp4.lt p44, p45 = -33, r3
  cmp.lt p46, p47 = r2, r3
  cmp.le p48, p49 = r2, r3
  cmp4.ge p50, p51 = r2, r3
  tbit.z p52, p53 = r3, 20
  cmp.gtu p54, p55 = r2, r3
  cmp4.ne p56, p57 = r2, r3
  cmp.ge p58, p59 = 44, r3
  cmp4.leu p60, p61 = r2, r3
  cmp.geu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
