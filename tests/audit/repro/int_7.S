/* SPDX-License-Identifier: GPL-2.0-or-later */
// EXPECT r8 = 0x80000000   add R = r2, r3
// EXPECT r9 = 0x80000001   add R = r2, r3, 1
// EXPECT r10 = 0xffffffff80000002   sub R = r2, r3
// EXPECT r11 = 0xffffffff80000001   sub R = r2, r3, 1
// EXPECT r12 = 0xffffffff7fffffbd   sub R = -68, r3
// EXPECT r13 = 0x7fffffbc   and R = -68, r3
// EXPECT r14 = 0xffffffffffffffff   or R = -68, r3
// EXPECT r15 = 0xffffffff80000043   xor R = -68, r3
// EXPECT r16 = 0xffffffff80000000   andcm R = -68, r3
// EXPECT r17 = 0x1   and R = r2, r3
// EXPECT r18 = 0x7fffffff   or R = r2, r3
// EXPECT r19 = 0x7ffffffe   xor R = r2, r3
// EXPECT r20 = 0x0   andcm R = r2, r3
// EXPECT r21 = 0x8000077b   adds R = 1916, r3
// EXPECT r22 = 0xfffffffffff7220a   addl R = -581110, r1
// EXPECT r23 = 0x80000001   shladd R = r2, 1, r3
// EXPECT r24 = 0x2000000080000001   shladdp4 R = r2, 1, r3
// EXPECT r25 = 0x80000003   shladd R = r2, 2, r3
// EXPECT r26 = 0x2000000080000003   shladdp4 R = r2, 2, r3
// EXPECT r27 = 0x80000007   shladd R = r2, 3, r3
// EXPECT r28 = 0x2000000080000007   shladdp4 R = r2, 3, r3
// EXPECT r29 = 0x8000000f   shladd R = r2, 4, r3
// EXPECT r30 = 0x200000008000000f   shladdp4 R = r2, 4, r3
// EXPECT r31 = 0x2000000080000000   addp4 R = r2, r3
// EXPECT r32 = 0x200000008000077b   addp4 R = 1916, r3
// EXPECT r33 = 0x0   shl R = r2, r4
// EXPECT r34 = 0x0   shr.u R = r3, r4
// EXPECT r35 = 0x0   shr R = r3, r4
// EXPECT r36 = 0x3ffff   extr.u R = r3, 13, 25
// EXPECT r37 = 0x3ffff   extr R = r3, 13, 25
// EXPECT r38 = 0x2000   dep.z R = r2, 13, 25
// EXPECT r39 = 0x7c000   dep.z R = 62, 13, 25
// EXPECT r40 = 0x7ffe3fff   dep R = r2, r3, 13, 4
// EXPECT r41 = 0x1fff   dep R = 0, r3, 13, 25
// EXPECT r42 = 0x200000000ffffff   shrp R = r2, r3, 7
// EXPECT r43 = 0x80   shl R = r2, 7
// EXPECT r44 = 0xffffff   shr.u R = r3, 7
// EXPECT r45 = 0xffffff   shr R = r3, 7
// EXPECT r46 = 0x1f   extr.u R = r3, 26, 64
// EXPECT r47 = 0x1f   extr R = r3, 26, 64
// EXPECT r48 = 0x4000000   dep.z R = r2, 26, 64
// EXPECT r49 = 0x168000000   dep.z R = 90, 26, 64
// EXPECT r50 = 0x7ffffff   dep R = r2, r3, 26, 11
// EXPECT r51 = 0xffffffffffffffff   dep R = -1, r3, 26, 64
// EXPECT r52 = 0x40   shrp R = r2, r3, 58
// EXPECT r53 = 0x400000000000000   shl R = r2, 58
// EXPECT r54 = 0x0   shr.u R = r3, 58
// EXPECT r55 = 0x0   shr R = r3, 58
// EXPECT r56 = 0x0   extr.u R = r3, 46, 39
// EXPECT r57 = 0x0   extr R = r3, 46, 39
// EXPECT r58 = 0x400000000000   dep.z R = r2, 46, 39
// EXPECT r59 = 0xffffc00000000000   dep.z R = -1, 46, 39
// EXPECT r60 = 0x40007fffffff   dep R = r2, r3, 46, 6
// EXPECT r61 = 0x7fffffff   dep R = 0, r3, 46, 39
// EXPECT r62 = 0x400000001fffff   shrp R = r2, r3, 10
// EXPECT r63 = 0x400   shl R = r2, 10
// EXPECT r64 = 0x1fffff   shr.u R = r3, 10
// EXPECT r65 = 0x1fffff   shr R = r3, 10
// EXPECT r66 = 0x0   extr.u R = r3, 38, 64
// EXPECT r67 = 0x0   extr R = r3, 38, 64
// EXPECT r68 = 0x4000000000   dep.z R = r2, 38, 64
// EXPECT r69 = 0xbc000000000   dep.z R = 47, 38, 64
// EXPECT r70 = 0x407fffffff   dep R = r2, r3, 38, 15
// EXPECT r71 = 0xffffffc07fffffff   dep R = -1, r3, 38, 64
// EXPECT r72 = 0x800000003fffff   shrp R = r2, r3, 9
// EXPECT r73 = 0x200   shl R = r2, 9
// EXPECT r74 = 0x3fffff   shr.u R = r3, 9
// EXPECT r75 = 0x3fffff   shr R = r3, 9
// EXPECT r76 = 0xffff   extr.u R = r3, 15, 54
// EXPECT r77 = 0xffff   extr R = r3, 15, 54
// EXPECT r78 = 0x8000   dep.z R = r2, 15, 54
// EXPECT r79 = 0xffffffffffea0000   dep.z R = -44, 15, 54
// EXPECT r80 = 0x7c00ffff   dep R = r2, r3, 15, 11
// EXPECT r81 = 0x7fff   dep R = 0, r3, 15, 54
// EXPECT r82 = 0x4   shrp R = r2, r3, 62
// EXPECT r83 = 0x4000000000000000   shl R = r2, 62
// EXPECT r84 = 0x0   shr.u R = r3, 62
// EXPECT r85 = 0x0   shr R = r3, 62
// EXPECT r86 = 0x0   extr.u R = r3, 53, 6
// EXPECT r87 = 0x0   extr R = r3, 53, 6
// EXPECT r88 = 0x20000000000000   dep.z R = r2, 53, 6
// EXPECT r89 = 0x4e0000000000000   dep.z R = -89, 53, 6
// EXPECT r90 = 0x2000007fffffff   dep R = r2, r3, 53, 11
// EXPECT r91 = 0x7e000007fffffff   dep R = -1, r3, 53, 6
// EXPECT r92 = 0x100000   shrp R = r2, r3, 44
// EXPECT r93 = 0x100000000000   shl R = r2, 44
// EXPECT r94 = 0x0   shr.u R = r3, 44
// EXPECT r95 = 0x0   shr R = r3, 44
// EXPECT r96 = 0xffffffffffffffff   sxt1 R = r3
// EXPECT r97 = 0xffffffffffffffff   sxt2 R = r3
// EXPECT r98 = 0x7fffffff   sxt4 R = r3
// EXPECT r99 = 0xff   zxt1 R = r3
// EXPECT r100 = 0xffff   zxt2 R = r3
// EXPECT r101 = 0x7fffffff   zxt4 R = r3
// EXPECT r102 = 0x1f   popcnt R = r3
// EXPECT p6 = 1   cmp4.lt P1, P2 = 49, r3
// EXPECT p7 = 0
// EXPECT p8 = 1   tbit.z P1, P2 = r3, 31
// EXPECT p9 = 0
// EXPECT p10 = 1   cmp4.leu P1, P2 = 53, r3
// EXPECT p11 = 0
// EXPECT p12 = 1   cmp.lt P1, P2 = -93, r3
// EXPECT p13 = 0
// EXPECT p14 = 1   cmp.leu P1, P2 = r2, r3
// EXPECT p15 = 0
// EXPECT p16 = 1   cmp4.le P1, P2 = r2, r3
// EXPECT p17 = 0
// EXPECT p18 = 0   tbit.nz P1, P2 = r3, 31
// EXPECT p19 = 1
// EXPECT p20 = 1   cmp4.gtu P1, P2 = -69, r3
// EXPECT p21 = 0
// EXPECT p22 = 0   cmp.ge P1, P2 = 30, r3
// EXPECT p23 = 1
// EXPECT p24 = 1   cmp.le P1, P2 = r2, r3
// EXPECT p25 = 0
// EXPECT p26 = 0   cmp4.ltu P1, P2 = -117, r3
// EXPECT p27 = 1
// EXPECT p28 = 1   tbit.nz P1, P2 = r3, 27
// EXPECT p29 = 0
// EXPECT p30 = 1   cmp4.geu P1, P2 = -98, r3
// EXPECT p31 = 0
// EXPECT p32 = 1   cmp4.ltu P1, P2 = r2, r3
// EXPECT p33 = 0
// EXPECT p34 = 1   cmp4.ne P1, P2 = r2, r3
// EXPECT p35 = 0
// EXPECT p36 = 1   cmp4.lt P1, P2 = r2, r3
// EXPECT p37 = 0
// EXPECT p38 = 0   cmp.gtu P1, P2 = r2, r3
// EXPECT p39 = 1
// EXPECT p40 = 1   cmp.lt P1, P2 = r2, r3
// EXPECT p41 = 0
// EXPECT p42 = 1   cmp.ne P1, P2 = r2, r3
// EXPECT p43 = 0
// EXPECT p44 = 1   tbit.nz P1, P2 = r3, 16
// EXPECT p45 = 0
// EXPECT p46 = 1   cmp4.leu P1, P2 = r2, r3
// EXPECT p47 = 0
// EXPECT p48 = 0   cmp.ltu P1, P2 = -81, r3
// EXPECT p49 = 1
// EXPECT p50 = 0   cmp4.eq P1, P2 = 17, r3
// EXPECT p51 = 1
// EXPECT p52 = 0   cmp4.gtu P1, P2 = r2, r3
// EXPECT p53 = 1
// EXPECT p54 = 0   cmp.eq P1, P2 = r2, r3
// EXPECT p55 = 1
// EXPECT p56 = 1   cmp.gtu P1, P2 = -97, r3
// EXPECT p57 = 0
// EXPECT p58 = 0   cmp.geu P1, P2 = r2, r3
// EXPECT p59 = 1
// EXPECT p60 = 0   tbit.nz P1, P2 = r3, 36
// EXPECT p61 = 1
// EXPECT p62 = 0   cmp4.geu P1, P2 = r2, r3
// EXPECT p63 = 1
.text
.global _start
_start:
  movl r2 = 0x1
  movl r3 = 0x7fffffff
  movl r4 = 0x8000000000000000
  ;;
  add r8 = r2, r3
  add r9 = r2, r3, 1
  sub r10 = r2, r3
  sub r11 = r2, r3, 1
  sub r12 = -68, r3
  and r13 = -68, r3
  or r14 = -68, r3
  xor r15 = -68, r3
  andcm r16 = -68, r3
  and r17 = r2, r3
  or r18 = r2, r3
  xor r19 = r2, r3
  andcm r20 = r2, r3
  adds r21 = 1916, r3
  addl r22 = -581110, r1
  shladd r23 = r2, 1, r3
  shladdp4 r24 = r2, 1, r3
  shladd r25 = r2, 2, r3
  shladdp4 r26 = r2, 2, r3
  shladd r27 = r2, 3, r3
  shladdp4 r28 = r2, 3, r3
  shladd r29 = r2, 4, r3
  shladdp4 r30 = r2, 4, r3
  addp4 r31 = r2, r3
  addp4 r32 = 1916, r3
  shl r33 = r2, r4
  shr.u r34 = r3, r4
  shr r35 = r3, r4
  extr.u r36 = r3, 13, 25
  extr r37 = r3, 13, 25
  dep.z r38 = r2, 13, 25
  dep.z r39 = 62, 13, 25
  dep r40 = r2, r3, 13, 4
  dep r41 = 0, r3, 13, 25
  shrp r42 = r2, r3, 7
  shl r43 = r2, 7
  shr.u r44 = r3, 7
  shr r45 = r3, 7
  extr.u r46 = r3, 26, 64
  extr r47 = r3, 26, 64
  dep.z r48 = r2, 26, 64
  dep.z r49 = 90, 26, 64
  dep r50 = r2, r3, 26, 11
  dep r51 = -1, r3, 26, 64
  shrp r52 = r2, r3, 58
  shl r53 = r2, 58
  shr.u r54 = r3, 58
  shr r55 = r3, 58
  extr.u r56 = r3, 46, 39
  extr r57 = r3, 46, 39
  dep.z r58 = r2, 46, 39
  dep.z r59 = -1, 46, 39
  dep r60 = r2, r3, 46, 6
  dep r61 = 0, r3, 46, 39
  shrp r62 = r2, r3, 10
  shl r63 = r2, 10
  shr.u r64 = r3, 10
  shr r65 = r3, 10
  extr.u r66 = r3, 38, 64
  extr r67 = r3, 38, 64
  dep.z r68 = r2, 38, 64
  dep.z r69 = 47, 38, 64
  dep r70 = r2, r3, 38, 15
  dep r71 = -1, r3, 38, 64
  shrp r72 = r2, r3, 9
  shl r73 = r2, 9
  shr.u r74 = r3, 9
  shr r75 = r3, 9
  extr.u r76 = r3, 15, 54
  extr r77 = r3, 15, 54
  dep.z r78 = r2, 15, 54
  dep.z r79 = -44, 15, 54
  dep r80 = r2, r3, 15, 11
  dep r81 = 0, r3, 15, 54
  shrp r82 = r2, r3, 62
  shl r83 = r2, 62
  shr.u r84 = r3, 62
  shr r85 = r3, 62
  extr.u r86 = r3, 53, 6
  extr r87 = r3, 53, 6
  dep.z r88 = r2, 53, 6
  dep.z r89 = -89, 53, 6
  dep r90 = r2, r3, 53, 11
  dep r91 = -1, r3, 53, 6
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
  cmp4.lt p6, p7 = 49, r3
  tbit.z p8, p9 = r3, 31
  cmp4.leu p10, p11 = 53, r3
  cmp.lt p12, p13 = -93, r3
  cmp.leu p14, p15 = r2, r3
  cmp4.le p16, p17 = r2, r3
  tbit.nz p18, p19 = r3, 31
  cmp4.gtu p20, p21 = -69, r3
  cmp.ge p22, p23 = 30, r3
  cmp.le p24, p25 = r2, r3
  cmp4.ltu p26, p27 = -117, r3
  tbit.nz p28, p29 = r3, 27
  cmp4.geu p30, p31 = -98, r3
  cmp4.ltu p32, p33 = r2, r3
  cmp4.ne p34, p35 = r2, r3
  cmp4.lt p36, p37 = r2, r3
  cmp.gtu p38, p39 = r2, r3
  cmp.lt p40, p41 = r2, r3
  cmp.ne p42, p43 = r2, r3
  tbit.nz p44, p45 = r3, 16
  cmp4.leu p46, p47 = r2, r3
  cmp.ltu p48, p49 = -81, r3
  cmp4.eq p50, p51 = 17, r3
  cmp4.gtu p52, p53 = r2, r3
  cmp.eq p54, p55 = r2, r3
  cmp.gtu p56, p57 = -97, r3
  cmp.geu p58, p59 = r2, r3
  tbit.nz p60, p61 = r3, 36
  cmp4.geu p62, p63 = r2, r3
  ;;
  break.i 0x1ffffe
